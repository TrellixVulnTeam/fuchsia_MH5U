// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        error::Error,
        extent::Extent,
        format::ExtentClusterHeader,
        options::ExtractorOptions,
        properties::{DataKind, ExtentKind},
        utils::{RangeOps, ReadAndSeek},
    },
    interval_tree::interval_tree::IntervalTree,
    std::io::{Read, Write},
};

/// Returns `true` if the extent's data needs to be dumped.
fn should_dump_data(extent: &Extent, dump_pii: bool) -> bool {
    let properties = extent.properties();
    // For ExtentKind::Data, we do not need to dump data when DataKind is
    // either Skipped or it is Zeroes.
    // We dump Pii only when we forced to dump Pii or it was `Modified` by the
    // storage software to obfuscate the Pii data.
    match properties.extent_kind {
        ExtentKind::Data => match properties.data_kind {
            DataKind::Modified => true,
            DataKind::Unmodified => true,
            _ => false,
        },
        ExtentKind::Pii => match properties.data_kind {
            DataKind::Modified => true,
            DataKind::Unmodified => dump_pii,
            _ => false,
        },
        _ => false,
    }
}

/// This is in-memory representation of a collection of Extents.
///
/// We hold extents in memory, (coalescing, splitting, merging if needed), till
/// write() is issued.
/// The cluster of extents lives in image file in contiguous location.
#[derive(Debug)]
pub struct ExtentCluster {
    pub(crate) extent_tree: IntervalTree<Extent, u64>,
    options: ExtractorOptions,
}

impl ExtentCluster {
    pub fn new(options: &ExtractorOptions) -> Self {
        Self { extent_tree: IntervalTree::new(), options: options.clone() }
    }

    /// Adds an extent to extent cluster.
    pub fn add_extent(&mut self, extent: &Extent) -> Result<(), Error> {
        if !extent.storage_range().is_valid() {
            return Err(Error::InvalidRange);
        }
        self.extent_tree.add_interval(extent).map_err(|e| Error::from(e))
    }

    /// Returns number of extent in extent cluster.
    pub fn extent_count(&self) -> u64 {
        self.extent_tree.interval_count() as u64
    }

    /// Returns number of data bytes that will be written
    /// in this cluster.
    fn data_size(&self) -> u64 {
        let mut size: u64 = 0;
        for (_, extent) in self.extent_tree.get_iter() {
            if !should_dump_data(&extent, self.options.force_dump_pii) {
                continue;
            }
            size = size + extent.storage_range().length();
        }
        size
    }

    /// Writes ExtentCluster metadata to out_stream. crc32 to is crc of metadata and any
    /// padding required.
    fn write_metadata(&self, out_stream: &mut dyn Write) -> Result<u64, Error> {
        let mut size =
            ExtentClusterHeader::serialize_to(self.extent_count() as u64, 0, out_stream)?;

        for (_, extent) in self.extent_tree.get_iter() {
            size = size + extent.write(out_stream)?;
        }
        let zero_fill_len = (((size + self.options.alignment - 1) / self.options.alignment)
            * self.options.alignment)
            - size;
        let zeroes: Vec<u8> = vec![0; zero_fill_len as usize];
        out_stream.write_all(&zeroes).map_err(move |_| Error::WriteFailed)?;
        Ok(size + zero_fill_len)
    }

    /// Write all the data in extents to the out_stream.
    ///
    /// The extent's data is read from the in_stream.
    fn write_data(
        &self,
        in_stream: &mut dyn ReadAndSeek,
        out_stream: &mut dyn Write,
    ) -> Result<u64, Error> {
        let mut size = 0;
        for (_, extent) in self.extent_tree.get_iter() {
            // No need to dump the data. Only ExtentInfo will be written to the image file.
            if !should_dump_data(&extent, self.options.force_dump_pii) {
                continue;
            }
            let bytes_to_read = extent.storage_range().length();
            let mut read_buffer: Vec<u8> = vec![0; bytes_to_read as usize];
            let offset = extent.storage_range().start;
            // Seek to the storage location.
            in_stream.seek(std::io::SeekFrom::Start(offset)).map_err(move |_| Error::SeekFailed)?;
            let mut r = in_stream.take(bytes_to_read);

            // Read from the storage.
            r.read(&mut read_buffer[..bytes_to_read as usize]).map_err(move |_| {
                eprintln!(
                    "Failed to read {} bytes from {} offset for {:?}",
                    bytes_to_read, offset, extent
                );
                Error::ReadFailed
            })?;

            // Write to the image file
            out_stream.write_all(&read_buffer).map_err(move |_| Error::WriteFailed)?;
            size = size + bytes_to_read;
        }
        Ok(size)
    }

    /// Writes extent cluster to the image file.
    ///
    /// # Arguments
    ///
    /// `out_stream`    : Points to the image file stream.
    /// `in_stream`     : Stream from where extent data will be read from.
    /// `current_offset`: Starting position of the cluster within the image file.
    /// `last_cluster`  : True if this is the last cluster within the extracted image file.
    ///                   Multi-cluster image files are not yet supported.
    ///
    /// This includes computing checksum of the cluster header and all the extent infos,
    /// then writing cluster header, extent info and extent data to the image file.
    pub fn write(
        &mut self,
        mut in_stream: &mut dyn ReadAndSeek,
        _current_offset: u64,
        last_cluster: bool,
        mut out_stream: &mut dyn Write,
    ) -> Result<u64, Error> {
        if !last_cluster {
            todo!("Support for more than one cluster is not implemented.")
        }

        self.extent_tree.check_interval_tree();
        // Update all the extent location w.r.t. starting of the extent cluster.
        let _data_size = self.data_size();

        let mut size = self.write_metadata(out_stream)?;
        size = size + self.write_data(&mut in_stream, &mut out_stream)?;
        Ok(size)
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::{
            extent_cluster::ExtentCluster,
            properties::{DataKind, ExtentKind, ExtentProperties},
        },
        interval_tree::interval::Interval,
        std::io::Cursor,
        std::ops::Range,
    };

    static INSERTED_ADDRESS_START: u64 = 20;
    static INSERTED_ADDRESS_END: u64 = 30;

    static LOW_PRIORITY_PROPERTIES: ExtentProperties =
        ExtentProperties { extent_kind: ExtentKind::Data, data_kind: DataKind::Zeroes };

    static HIGH_PRIORITY_PROPERTIES: ExtentProperties =
        ExtentProperties { extent_kind: ExtentKind::Pii, data_kind: DataKind::Zeroes };

    static INSERTED_PROPERTIES: ExtentProperties =
        ExtentProperties { extent_kind: ExtentKind::Data, data_kind: DataKind::Unmodified };

    static OVERLAPPING_RIGHT_ADDRESS: Range<u64> =
        INSERTED_ADDRESS_END - 5..INSERTED_ADDRESS_END + 6;

    fn inserted_range() -> Range<u64> {
        INSERTED_ADDRESS_START..INSERTED_ADDRESS_END
    }

    fn inserted_extent() -> Extent {
        Extent::new(
            inserted_range(),
            INSERTED_PROPERTIES,
            // data: Some(vec![1; inserted_range().length() as usize]),
            None,
        )
        .unwrap()
    }

    // Non-overlapping extent to the right of inserted_extent().
    fn right_extent() -> Extent {
        Extent::new(
            INSERTED_ADDRESS_END + 5..INSERTED_ADDRESS_END + 15,
            INSERTED_PROPERTIES,
            // data: Some(vec![1; inserted_range().length() as usize]),
            None,
        )
        .unwrap()
    }
    fn setup_extent_cluster() -> (ExtentCluster, Vec<Extent>) {
        let mut cluster = ExtentCluster::new(&Default::default());
        match cluster.add_extent(&inserted_extent()) {
            Err(why) => println!("why: {:?}", why),
            Ok(_) => {}
        }
        return (cluster, vec![inserted_extent().clone()]);
    }

    fn verify(file: &str, line: u32, cluster: &ExtentCluster, extents: &Vec<Extent>) {
        if cluster.extent_tree.interval_count() != extents.len() {
            println!("{}:{} Expected: {:?}\nFound: {:?}", file, line, extents, cluster.extent_tree);
        }
        assert_eq!(cluster.extent_tree.interval_count(), extents.len());

        for (_, ext) in cluster.extent_tree.get_iter() {
            let mut found = false;
            for inserted in extents.iter() {
                if inserted == ext {
                    found = true;
                    break;
                }
            }
            if !found {
                println!("Could not find {:?}", *ext);
            }
            assert!(found);
        }
    }

    #[test]
    fn test_setup() {
        let (cluster, expected_extents) = setup_extent_cluster();
        assert!(INSERTED_PROPERTIES > LOW_PRIORITY_PROPERTIES);
        assert!(INSERTED_PROPERTIES < HIGH_PRIORITY_PROPERTIES);
        assert!(LOW_PRIORITY_PROPERTIES < HIGH_PRIORITY_PROPERTIES);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_non_overlapping() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        let e = Extent::new(40..50, INSERTED_PROPERTIES, None).unwrap();
        cluster.add_extent(&e).unwrap();
        expected_extents.push(e);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_is_adjacent_not_mergable() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        let e = Extent::new(
            INSERTED_ADDRESS_END..INSERTED_ADDRESS_END + 5,
            LOW_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&e).unwrap();
        expected_extents.push(e);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_is_adjacent() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        let e =
            Extent::new(INSERTED_ADDRESS_END..INSERTED_ADDRESS_END + 5, INSERTED_PROPERTIES, None)
                .unwrap();
        cluster.add_extent(&e).unwrap();
        expected_extents.clear();
        expected_extents.push(
            Extent::new(
                INSERTED_ADDRESS_START..INSERTED_ADDRESS_END + 5,
                INSERTED_PROPERTIES,
                None,
            )
            .unwrap(),
        );
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_adjacent_in_the_middle() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster.add_extent(&right_extent()).unwrap();
        expected_extents.push(right_extent().clone());
        verify(file!(), line!(), &cluster, &expected_extents);
        let e =
            Extent::new(INSERTED_ADDRESS_END..INSERTED_ADDRESS_END + 5, INSERTED_PROPERTIES, None)
                .unwrap();
        cluster.add_extent(&e).unwrap();
        expected_extents.clear();
        expected_extents.push(
            Extent::new(
                INSERTED_ADDRESS_START..INSERTED_ADDRESS_END + 15,
                INSERTED_PROPERTIES,
                None,
            )
            .unwrap(),
        );
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_adjacent_in_the_middle_not_mergable() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster.add_extent(&right_extent()).unwrap();
        expected_extents.push(right_extent().clone());
        verify(file!(), line!(), &cluster, &expected_extents);
        let m = Extent::new(
            INSERTED_ADDRESS_END..INSERTED_ADDRESS_END + 5,
            HIGH_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&m).unwrap();
        expected_extents.push(m);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_adjacent_on_both_sides_overlapping_in_middle() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        // Adjacent to left
        let mut m = Extent::new(
            INSERTED_ADDRESS_START - 10..INSERTED_ADDRESS_START,
            LOW_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&m).unwrap();
        expected_extents.push(m.clone());
        m.set_start(INSERTED_ADDRESS_END);
        m.set_end(INSERTED_ADDRESS_END + 10);
        cluster.add_extent(&m).unwrap();
        expected_extents.push(m.clone());

        verify(file!(), line!(), &cluster, &expected_extents);

        let m = Extent::new(
            INSERTED_ADDRESS_START..INSERTED_ADDRESS_END,
            LOW_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&m).unwrap();
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster
            .add_extent(
                &Extent::new(OVERLAPPING_RIGHT_ADDRESS.clone(), INSERTED_PROPERTIES, None).unwrap(),
            )
            .unwrap();
        expected_extents[0].set_end(OVERLAPPING_RIGHT_ADDRESS.end);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping_low_priority() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        let mut e =
            Extent::new(OVERLAPPING_RIGHT_ADDRESS.clone(), LOW_PRIORITY_PROPERTIES, None).unwrap();
        cluster.add_extent(&e).unwrap();
        e.set_start(INSERTED_ADDRESS_END);
        expected_extents.push(e);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping_high_priority() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster
            .add_extent(
                &Extent::new(OVERLAPPING_RIGHT_ADDRESS.clone(), HIGH_PRIORITY_PROPERTIES, None)
                    .unwrap(),
            )
            .unwrap();
        expected_extents[0].set_end(OVERLAPPING_RIGHT_ADDRESS.start);
        expected_extents.push(
            Extent::new(OVERLAPPING_RIGHT_ADDRESS.clone(), HIGH_PRIORITY_PROPERTIES, None).unwrap(),
        );
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping_in_middlex() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster.add_extent(&right_extent()).unwrap();
        expected_extents.push(right_extent().clone());
        verify(file!(), line!(), &cluster, &expected_extents);

        let middle_extent =
            Extent::new(OVERLAPPING_RIGHT_ADDRESS.clone(), INSERTED_PROPERTIES, None).unwrap();
        cluster.add_extent(&middle_extent).unwrap();
        expected_extents.clear();
        expected_extents.push(
            Extent::new(INSERTED_ADDRESS_START..right_extent().end(), INSERTED_PROPERTIES, None)
                .unwrap(),
        );
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping_in_middle_low_priority() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster.add_extent(&right_extent()).unwrap();
        expected_extents.push(right_extent().clone());
        verify(file!(), line!(), &cluster, &expected_extents);

        cluster
            .add_extent(
                &Extent::new(OVERLAPPING_RIGHT_ADDRESS.clone(), LOW_PRIORITY_PROPERTIES, None)
                    .unwrap(),
            )
            .unwrap();
        expected_extents.push(
            Extent::new(
                INSERTED_ADDRESS_END..INSERTED_ADDRESS_END + 5,
                LOW_PRIORITY_PROPERTIES,
                None,
            )
            .unwrap(),
        );
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping_in_middle_high_priority() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster.add_extent(&right_extent()).unwrap();
        expected_extents.push(right_extent().clone());
        verify(file!(), line!(), &cluster, &expected_extents);

        let middle_extent =
            Extent::new(OVERLAPPING_RIGHT_ADDRESS.clone(), HIGH_PRIORITY_PROPERTIES, None).unwrap();
        cluster.add_extent(&middle_extent).unwrap();
        expected_extents[0].set_end(OVERLAPPING_RIGHT_ADDRESS.start);
        expected_extents[1].set_start(OVERLAPPING_RIGHT_ADDRESS.end);
        expected_extents.push(middle_extent);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping_multiple() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster.add_extent(&right_extent()).unwrap();
        expected_extents.push(right_extent().clone());
        let extreme_right_extent = Extent::new(
            right_extent().end() + 10..right_extent().end() + 20,
            INSERTED_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&extreme_right_extent).unwrap();
        expected_extents.push(extreme_right_extent.clone());
        verify(file!(), line!(), &cluster, &expected_extents);

        let overlapping_extent = Extent::new(
            INSERTED_ADDRESS_START - 5..extreme_right_extent.end() + 10,
            INSERTED_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&overlapping_extent).unwrap();
        expected_extents.clear();
        expected_extents.push(overlapping_extent);
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_overlapping_multiple_not_mergeable() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        cluster.add_extent(&right_extent()).unwrap();
        expected_extents.push(right_extent().clone());
        let extreme_right_extent = Extent::new(
            right_extent().end() + 10..right_extent().end() + 20,
            INSERTED_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&extreme_right_extent).unwrap();
        expected_extents.push(extreme_right_extent.clone());
        verify(file!(), line!(), &cluster, &expected_extents);

        let overlapping_extent = Extent::new(
            INSERTED_ADDRESS_START - 5..extreme_right_extent.end() + 10,
            LOW_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&overlapping_extent).unwrap();
        // The overlapping_extent gets divided into multiple extents.
        let mut split_extent = overlapping_extent.clone();
        split_extent.set_end(inserted_extent().start());
        expected_extents.push(split_extent.clone());
        split_extent.set_start(inserted_extent().end());
        split_extent.set_end(right_extent().start());
        expected_extents.push(split_extent.clone());
        split_extent.set_start(right_extent().end());
        split_extent.set_end(extreme_right_extent.start());
        expected_extents.push(split_extent.clone());
        split_extent.set_start(extreme_right_extent.end());
        split_extent.set_end(overlapping_extent.end());
        expected_extents.push(split_extent.clone());
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    #[test]
    fn test_add_splits_existing_extent() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        let small_extent = Extent::new(
            inserted_extent().start() + 3..inserted_extent().end() - 3,
            HIGH_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&small_extent).unwrap();
        expected_extents.push(expected_extents[0].clone());
        expected_extents[0].set_end(small_extent.start());
        expected_extents[1].set_start(small_extent.end());
        expected_extents.push(small_extent.clone());
        verify(file!(), line!(), &cluster, &expected_extents);
    }
    #[test]
    fn test_add_splits_existing_extent_at_start() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        let small_extent = Extent::new(
            inserted_extent().start()..inserted_extent().end() - 3,
            HIGH_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&small_extent).unwrap();
        expected_extents[0].set_start(small_extent.end());
        expected_extents.push(small_extent.clone());
        verify(file!(), line!(), &cluster, &expected_extents);
    }
    #[test]
    fn test_add_splits_existing_extent_at_end() {
        let (mut cluster, mut expected_extents) = setup_extent_cluster();
        let small_extent = Extent::new(
            inserted_extent().start() + 3..inserted_extent().end(),
            HIGH_PRIORITY_PROPERTIES,
            None,
        )
        .unwrap();
        cluster.add_extent(&small_extent).unwrap();
        expected_extents[0].set_end(small_extent.start());
        expected_extents.push(small_extent.clone());
        verify(file!(), line!(), &cluster, &expected_extents);
    }

    fn dumpable_data_properties() -> ExtentProperties {
        ExtentProperties { extent_kind: ExtentKind::Data, data_kind: DataKind::Modified }
    }

    fn skippable_data_properties() -> ExtentProperties {
        ExtentProperties { extent_kind: ExtentKind::Unmmapped, data_kind: DataKind::Skipped }
    }

    fn pii_data_properties() -> ExtentProperties {
        ExtentProperties { extent_kind: ExtentKind::Pii, data_kind: DataKind::Unmodified }
    }

    fn setup_cluster_write_test(
        dump_pii: bool,
    ) -> (ExtentCluster, ExtractorOptions, Vec<u8>, Cursor<Vec<u8>>) {
        let mut options: ExtractorOptions = Default::default();
        options.force_dump_pii = dump_pii;
        let cluster = ExtentCluster::new(&options);
        let out_buffer: Vec<u8> = vec![];
        let in_buffer = Cursor::new(vec![0; 2 * 1024 * 1024]);

        (cluster, options, out_buffer, in_buffer)
    }

    #[test]
    fn test_extent_cluster_write() {
        let (mut cluster, options, mut out_buffer, mut in_buffer) = setup_cluster_write_test(false);

        let size = cluster.write(&mut in_buffer, 0, true, &mut out_buffer).unwrap();
        assert!(size > 0);
        assert_eq!(size % options.alignment, 0);
        assert_eq!(out_buffer.len() as u64, size);
    }

    #[test]
    fn test_extent_cluster_write_no_data() {
        let (mut cluster, options, mut out_buffer, mut in_buffer) = setup_cluster_write_test(false);
        let size = cluster.write(&mut in_buffer, 0, true, &mut out_buffer).unwrap();
        assert!(size > 0);
        assert_eq!(size % options.alignment, 0);
        assert_eq!(out_buffer.len() as u64, size);

        let properties = skippable_data_properties();
        let mut extent = Extent::new(0..1, properties, None).unwrap();

        // Write as many extents as it takes to fill one alignment unit.
        let extent_count = options.alignment / extent.serialized_size() as u64;
        for i in 0..extent_count {
            let start = i * 2 * options.alignment;
            extent.set_start(start);
            extent.set_end(start + options.alignment);
            cluster.add_extent(&extent).unwrap();
        }

        let mut new_buffer: Vec<u8> = vec![];
        let new_size = cluster.write(&mut in_buffer, 0, true, &mut new_buffer).unwrap();
        assert!(new_size > size);
        assert_eq!(new_size % options.alignment, 0);
        assert_eq!(new_buffer.len() as u64, new_size);
    }

    #[test]
    fn test_extent_cluster_write_with_data() {
        let (mut cluster, options, mut out_buffer, mut in_buffer) = setup_cluster_write_test(false);

        let no_data_extent =
            Extent::new(8192..(2 * 8192), skippable_data_properties(), None).unwrap();
        let data_extent1 =
            Extent::new((2 * 8192)..(3 * 8192), dumpable_data_properties(), None).unwrap();
        let data_extent2 =
            Extent::new((5 * 8192)..(6 * 8192), dumpable_data_properties(), None).unwrap();
        let pii_extent = Extent::new(8 * 8192..(9 * 8192), pii_data_properties(), None).unwrap();

        cluster.add_extent(&no_data_extent).unwrap();
        cluster.add_extent(&data_extent1).unwrap();
        cluster.add_extent(&data_extent2).unwrap();
        cluster.add_extent(&pii_extent).unwrap();

        let new_size = cluster.write(&mut in_buffer, 0, true, &mut out_buffer).unwrap();
        assert_eq!(new_size % options.alignment, 0);
        assert_eq!(out_buffer.len() as u64, new_size);

        // We end up writing three aligned segments.
        // - one cluster header and extents
        // - two data blocks
        assert_eq!(new_size, options.alignment * 3)
    }

    #[test]
    fn test_extent_cluster_write_with_pii() {
        let (mut cluster, options, mut out_buffer, mut in_buffer) = setup_cluster_write_test(true);

        // Add skippable data followed by pii at same range.
        let no_data_extent =
            Extent::new(8192..(15 * 8192), skippable_data_properties(), None).unwrap();
        let pii_extent = Extent::new(8192..(5 * 8192), pii_data_properties(), None).unwrap();

        cluster.add_extent(&no_data_extent).unwrap();
        cluster.add_extent(&pii_extent).unwrap();

        let new_size = cluster.write(&mut in_buffer, 0, true, &mut out_buffer).unwrap();
        assert_eq!(new_size % options.alignment, 0);
        assert_eq!(out_buffer.len() as u64, new_size);

        // We end up writing three aligned segments.
        // - one cluster header and extents
        // - four for pii blocks
        assert_eq!(new_size, options.alignment * 5)
    }

    fn verify_should_dump_data(ekind: ExtentKind, dkind: DataKind, dump_pii: bool, dump: bool) {
        let properties = ExtentProperties { extent_kind: ekind, data_kind: dkind };
        let extent = Extent::new(4..10, properties, None).unwrap();
        assert_eq!(
            should_dump_data(&extent, dump_pii),
            dump,
            "{:?} {} {}",
            properties,
            dump_pii,
            dump
        );
    }

    #[test]
    fn test_should_dump_data() {
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Skipped, true, false);
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Zeroes, true, false);
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Unmodified, true, false);
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Modified, true, false);
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Skipped, false, false);
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Zeroes, false, false);
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Unmodified, false, false);
        verify_should_dump_data(ExtentKind::Unmmapped, DataKind::Modified, false, false);

        verify_should_dump_data(ExtentKind::Unused, DataKind::Skipped, true, false);
        verify_should_dump_data(ExtentKind::Unused, DataKind::Zeroes, true, false);
        verify_should_dump_data(ExtentKind::Unused, DataKind::Unmodified, true, false);
        verify_should_dump_data(ExtentKind::Unused, DataKind::Modified, true, false);
        verify_should_dump_data(ExtentKind::Unused, DataKind::Skipped, false, false);
        verify_should_dump_data(ExtentKind::Unused, DataKind::Zeroes, false, false);
        verify_should_dump_data(ExtentKind::Unused, DataKind::Unmodified, false, false);
        verify_should_dump_data(ExtentKind::Unused, DataKind::Modified, false, false);

        verify_should_dump_data(ExtentKind::Data, DataKind::Skipped, true, false);
        verify_should_dump_data(ExtentKind::Data, DataKind::Zeroes, true, false);
        verify_should_dump_data(ExtentKind::Data, DataKind::Unmodified, true, true);
        verify_should_dump_data(ExtentKind::Data, DataKind::Modified, true, true);
        verify_should_dump_data(ExtentKind::Data, DataKind::Skipped, false, false);
        verify_should_dump_data(ExtentKind::Data, DataKind::Zeroes, false, false);
        verify_should_dump_data(ExtentKind::Data, DataKind::Unmodified, false, true);
        verify_should_dump_data(ExtentKind::Data, DataKind::Modified, false, true);

        verify_should_dump_data(ExtentKind::Pii, DataKind::Skipped, true, false);
        verify_should_dump_data(ExtentKind::Pii, DataKind::Zeroes, true, false);
        verify_should_dump_data(ExtentKind::Pii, DataKind::Unmodified, true, true);
        verify_should_dump_data(ExtentKind::Pii, DataKind::Modified, true, true);
        verify_should_dump_data(ExtentKind::Pii, DataKind::Skipped, false, false);
        verify_should_dump_data(ExtentKind::Pii, DataKind::Zeroes, false, false);
        verify_should_dump_data(ExtentKind::Pii, DataKind::Unmodified, false, false);
        verify_should_dump_data(ExtentKind::Pii, DataKind::Modified, false, true);
    }
}
