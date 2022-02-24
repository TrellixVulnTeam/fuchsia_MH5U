// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! File implements IoPacket and Target for POSIX File like objects. Current
//! implementation limits itself to blocking calls.

use {
    crate::common_operations::{pread, pwrite},
    crate::io_packet::{IoPacket, IoPacketType, TimeInterval},
    crate::operations::{OperationType, PipelineStages},
    crate::target::{Error, Target, TargetOps, TargetType},
    log::debug,
    log::error,
    std::{
        fs::{File, OpenOptions},
        ops::Range,
        os::unix::io::AsRawFd,
        process,
        result::Result,
        sync::Arc,
        time::Instant,
    },
};

#[derive(Clone)]
pub struct FileIoPacket {
    // io_sequence_number is monotonically increasing number which doesn't
    // repeat for this run and for this generator. This is used to
    // order-replay load.
    io_sequence_number: u64,

    // This value represents the seed used to generate the contents of the
    // IoPacket.
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    seed: u64,

    // Start and finish timestamps for different stages of IO.
    stage_timestamps: [TimeInterval; PipelineStages::stage_count()],

    // Type of IO operation
    operation_type: OperationType,

    // Range within the file on which this IO will be performed on. May not
    // applicable to all operations ex. create
    offset_range: Range<u64>,

    // Result of the completed IO operation
    io_result: Option<Error>,

    // The target(file) on which IO will be performed
    target: TargetType,

    // Payload of the IO
    buffer: Vec<u8>,
}

impl FileIoPacket {
    pub fn new(
        operation_type: OperationType,
        io_sequence_number: u64,
        seed: u64,
        offset_range: Range<u64>,
        target: TargetType,
    ) -> FileIoPacket {
        let buffer = vec![0; offset_range.end as usize - offset_range.start as usize];
        FileIoPacket {
            operation_type,
            io_sequence_number,
            seed,
            stage_timestamps: [TimeInterval::new(); PipelineStages::stage_count()],
            offset_range: offset_range.clone(),
            io_result: None,
            target,
            buffer,
        }
    }
}

impl IoPacket for FileIoPacket {
    fn operation_type(&self) -> OperationType {
        self.operation_type
    }

    fn timestamp_stage_start(&mut self, stage: PipelineStages) {
        self.stage_timestamps[stage.stage_number()].start();
    }

    fn timestamp_stage_end(&mut self, stage: PipelineStages) {
        self.stage_timestamps[stage.stage_number()].end();
    }

    fn sequence_number(&self) -> u64 {
        self.io_sequence_number
    }

    fn stage_timestamps(&self) -> &[TimeInterval; PipelineStages::stage_count()] {
        &self.stage_timestamps
    }

    fn interval_to_u64(&self, stage: PipelineStages) -> (u64, u64) {
        self.stage_timestamps[stage.stage_number()].interval_to_u64(&self.target.start_instant())
    }

    fn io_offset_range(&self) -> Range<u64> {
        self.offset_range.clone()
    }

    fn do_io(&mut self) {
        self.target.clone().do_io(self)
    }

    fn is_complete(&self) -> bool {
        self.target.clone().is_complete(self)
    }

    fn verify_needs_io(&self) -> bool {
        self.target.clone().verify_needs_io(self)
    }

    fn generate_verify_io(&mut self) {
        self.target.clone().generate_verify_io(self)
    }

    fn verify(&mut self, verify_packet: &dyn IoPacket) -> bool {
        self.target.clone().verify(self, verify_packet)
    }

    fn get_error(&self) -> Result<(), Error> {
        match &self.io_result {
            Some(error) => Err(error.clone()),
            None => Ok(()),
        }
    }

    fn set_error(&mut self, io_error: Error) {
        self.io_result = Some(io_error);
    }

    fn buffer_mut(&mut self) -> &mut Vec<u8> {
        &mut self.buffer
    }

    fn buffer(&mut self) -> &Vec<u8> {
        &self.buffer
    }
}

pub struct FileBlockingTarget {
    /// File name
    #[allow(unused)]
    name: String,

    /// Open file descriptor
    file: File,

    /// Unique file id for this run and for this generator
    target_unique_id: u64,

    /// Range within which this Targets operates on the file
    offset_range: Range<u64>,

    start_instant: Instant,
}

impl FileBlockingTarget {
    // Create a new Target instance. Fails when opening an existing file fails.
    // TODO(auradkar): Open should be moved to setup phase when all operations
    // file are supported.
    pub fn new(
        name: String,
        target_unique_id: u64,
        offset_range: Range<u64>,
        start_instant: Instant,
    ) -> TargetType {
        let file = OpenOptions::new().write(true).read(true).append(false).open(&name).unwrap();
        Arc::new(Box::new(FileBlockingTarget {
            name,
            file,
            target_unique_id,
            offset_range,
            start_instant,
        }))
    }

    // pwrite the buffer in IoPacket at io_offset_range.
    fn write(&self, io_packet: &mut dyn IoPacket) {
        let offset_range = io_packet.io_offset_range().clone();

        if offset_range.start < self.offset_range.start || offset_range.end > self.offset_range.end
        {
            io_packet.set_error(Error::OffsetOutOfRange);
            return;
        }

        let raw_fd = self.file.as_raw_fd().clone();
        let b = io_packet.buffer_mut();

        let ret = pwrite(raw_fd, b, offset_range.start as i64);
        if let Err(err) = ret {
            return io_packet.set_error(err);
        }
    }

    // pread the buffer in IoPacket at io_offset_range.
    fn read(&self, io_packet: &mut dyn IoPacket) {
        let offset_range = io_packet.io_offset_range().clone();

        if offset_range.start < self.offset_range.start || offset_range.end > self.offset_range.end
        {
            io_packet.set_error(Error::OffsetOutOfRange);
            return;
        }

        let raw_fd = self.file.as_raw_fd().clone();
        let b = io_packet.buffer_mut();

        let ret = pread(raw_fd, b, offset_range.start as i64);
        if let Err(err) = ret {
            return io_packet.set_error(err);
        }
    }

    fn open(&self, io_packet: &mut dyn IoPacket) {
        error!("open not yet supported {}", io_packet.sequence_number());
        process::abort();
    }

    fn exit(&self, io_packet: &mut dyn IoPacket) {
        debug!("Nothing to do for exit path {}", io_packet.sequence_number());
    }
}

impl Target for FileBlockingTarget {
    fn setup(&mut self, _file_name: &String, _range: Range<u64>) -> Result<(), Error> {
        Ok(())
    }

    fn create_io_packet(
        &self,
        operation_type: OperationType,
        seq: u64,
        seed: u64,
        io_offset_range: Range<u64>,
        target: TargetType,
    ) -> IoPacketType {
        Box::new(FileIoPacket::new(operation_type, seq, seed, io_offset_range, target))
    }

    fn id(&self) -> u64 {
        self.target_unique_id
    }

    fn supported_ops() -> &'static TargetOps
    where
        Self: Sized,
    {
        // For now only writes are supported.
        &TargetOps { write: true, read: true, open: false }
    }

    fn allowed_ops() -> &'static TargetOps
    where
        Self: Sized,
    {
        // For now only writes are allowed.
        &TargetOps { write: true, read: true, open: false }
    }

    fn do_io(&self, io_packet: &mut dyn IoPacket) {
        match io_packet.operation_type() {
            OperationType::Write => self.write(io_packet),
            OperationType::Read => self.read(io_packet),
            OperationType::Open => self.open(io_packet),
            OperationType::Exit => self.exit(io_packet),
            _ => {
                error!("Unsupported operation");
                process::abort();
            }
        };
    }

    fn is_complete(&self, io_packet: &dyn IoPacket) -> bool {
        match io_packet.operation_type() {
            OperationType::Read
            | OperationType::Write
            | OperationType::Open
            | OperationType::Exit => true,
            _ => {
                error!("Complete for unsupported operation");
                process::abort();
            }
        }
    }

    fn verify_needs_io(&self, io_packet: &dyn IoPacket) -> bool {
        match io_packet.operation_type() {
            OperationType::Read
            | OperationType::Write
            | OperationType::Open
            | OperationType::Exit => false,
            _ => {
                error!("verify_needs_io for unsupported operation");
                process::abort();
            }
        }
    }

    fn generate_verify_io(&self, io_packet: &mut dyn IoPacket) {
        match io_packet.operation_type() {
            _ => {
                error!("generate_verify_io for unsupported operation");
                process::abort();
            }
        };
    }

    fn verify(&self, io_packet: &mut dyn IoPacket, _verify_packet: &dyn IoPacket) -> bool {
        match io_packet.operation_type() {
            OperationType::Read | OperationType::Write | OperationType::Exit => true,
            _ => {
                error!("verify for unsupported operation");
                process::abort();
            }
        }
    }

    fn start_instant(&self) -> Instant {
        self.start_instant
    }
}

#[cfg(test)]
mod tests {

    use {
        crate::file_target::FileBlockingTarget,
        crate::operations::OperationType,
        crate::target::{Error, TargetType},
        std::{fs, fs::File, time::Instant},
    };

    static FILE_LENGTH: u64 = 1 * 1024 * 1024; // 1 MiB

    fn setup(file_name: &String) -> TargetType {
        let f = File::create(&file_name).unwrap();
        f.set_len(FILE_LENGTH).unwrap();
        let start_instant = Instant::now();
        FileBlockingTarget::new(file_name.to_string(), 0, 0..FILE_LENGTH, start_instant)
    }

    fn teardown(file_name: &String) {
        fs::remove_file(file_name).unwrap();
    }

    #[test]
    fn simple_write() {
        let file_name = "/tmp/odu-file_target-simple_write-file01".to_string();

        let target = setup(&file_name);
        let mut io_packet =
            target.create_io_packet(OperationType::Write, 0, 0, 0..4096, target.clone());
        let mut _buffer = io_packet.buffer_mut();
        io_packet.do_io();
        assert_eq!(io_packet.is_complete(), true);
        io_packet.get_error().unwrap();
        teardown(&file_name);
    }

    #[test]
    fn write_failure() {
        let file_name = "/tmp/odu-file_target-write_failure-file01".to_string();

        let target = setup(&file_name);

        // Try to write beyond allowed offset range
        let mut io_packet = target.create_io_packet(
            OperationType::Write,
            0,
            0,
            (2 * FILE_LENGTH)..(3 * FILE_LENGTH),
            target.clone(),
        );
        let mut _buffer = io_packet.buffer_mut();
        io_packet.do_io();
        assert_eq!(io_packet.is_complete(), true);
        assert_eq!(io_packet.get_error().is_err(), true);
        assert_eq!(io_packet.get_error().err(), Some(Error::OffsetOutOfRange));
        teardown(&file_name);
    }

    #[test]
    fn simple_read() {
        let file_name = "/tmp/odu-file_target-simple_read-file01".to_string();

        let target = setup(&file_name);
        let mut io_packet =
            target.create_io_packet(OperationType::Read, 0, 0, 0..4096, target.clone());
        let mut _buffer = io_packet.buffer_mut();
        io_packet.do_io();
        assert_eq!(io_packet.is_complete(), true);
        io_packet.get_error().unwrap();
        teardown(&file_name);
    }

    #[test]
    fn read_failure_out_of_range() {
        let file_name = "/tmp/odu-file_target-read_failure-file01".to_string();

        let target = setup(&file_name);

        // Try to read beyond allowed offset range
        let mut io_packet = target.create_io_packet(
            OperationType::Read,
            0,
            0,
            (2 * FILE_LENGTH)..(3 * FILE_LENGTH),
            target.clone(),
        );
        let mut _buffer = io_packet.buffer_mut();
        io_packet.do_io();
        assert_eq!(io_packet.is_complete(), true);
        assert_eq!(io_packet.get_error().is_err(), true);
        assert_eq!(io_packet.get_error().err(), Some(Error::OffsetOutOfRange));
        teardown(&file_name);
    }
}
