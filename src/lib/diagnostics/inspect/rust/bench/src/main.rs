// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    argh::{FromArgValue, FromArgs},
    fidl,
    fidl_fuchsia_inspect::{TreeMarker, TreeProxy},
    fuchsia_inspect::{
        reader::snapshot::{Snapshot, SnapshotTree},
        reader::ReadableTree,
        ArithmeticArrayProperty, ArrayProperty, ExponentialHistogramParams, Heap,
        HistogramProperty, Inspector, LinearHistogramParams, Node, NumericProperty, Property,
    },
    fuchsia_trace as ftrace,
    fuchsia_trace_provider::trace_provider_create_with_fdio,
    futures::FutureExt,
    inspect_runtime::service::{spawn_tree_server, TreeServerSettings},
    mapped_vmo::Mapping,
    num::{pow, traits::FromPrimitive, One},
    parking_lot::Mutex,
    paste,
    std::{
        ops::{Add, Mul},
        sync::Arc,
    },
};

#[derive(FromArgs)]
#[argh(description = "Rust inspect benchmarks")]
struct Args {
    #[argh(option, description = "number of iterations", short = 'i', default = "500")]
    iterations: usize,

    #[argh(option, description = "which benchmark to run (writer, reader)")]
    benchmark: BenchmarkOption,
}

enum BenchmarkOption {
    Reader,
    Writer,
}

impl FromArgValue for BenchmarkOption {
    fn from_arg_value(value: &str) -> Result<Self, String> {
        match value {
            "reader" => Ok(BenchmarkOption::Reader),
            "writer" => Ok(BenchmarkOption::Writer),
            _ => Err(format!("Unknown benchmark \"{}\"", value)),
        }
    }
}

const NAME: &str = "name";

macro_rules! bench_numeric_property_fn {
    ($name:ident, $type:ty, $Property:expr) => {
        paste::paste! {
            fn [<bench_ $name _property>](root: &Node) {
                let property = {
                    ftrace::duration!("benchmark",
                                      concat!("Node::create_", stringify!($name), "_property"));
                    root.[<create_ $name>](NAME, 0 as $type)
                };
                {
                    ftrace::duration!("benchmark", concat!($Property, "::set"));
                    property.set(1 as $type);
                }
                {
                    ftrace::duration!("benchmark", concat!($Property, "::add"));
                    property.add(1 as $type);
                }
                {
                    ftrace::duration!("benchmark", concat!($Property, "::subtract"));
                    property.subtract(1 as $type);
                }
                {
                    ftrace::duration!("benchmark", concat!($Property, "::drop"));
                    drop(property);
                }
                {
                    ftrace::duration!(
                        "benchmark",
                        concat!("Node::record_", stringify!($name), "_property"));
                    root.[<record_ $name>](NAME, 0 as $type);
                }
            }
        }
    };
}

macro_rules! bench_arithmetic_array_fn_impls {
    ($name:ident, $type:ty, $Array:expr, [$($size:expr),*]) => {
        $(
            paste::paste! {
                fn [<bench_ $name _array_ $size>](root: &Node, iteration: usize) {
                    let index = iteration % $size;
                    let array = {
                        ftrace::duration!(
                            "benchmark",
                            concat!("Node::create_", stringify!($name), "_array/", $size));
                        root.[<create_ $name _array>](NAME, $size)
                    };
                    {
                        ftrace::duration!("benchmark", concat!($Array, "::set/", $size));
                        array.set(index, 1 as $type);
                    }
                    {
                        ftrace::duration!("benchmark", concat!($Array, "::add/", $size));
                        array.add(index, 1 as $type);
                    }
                    {
                        ftrace::duration!("benchmark", concat!($Array, "::subtract/", $size));
                        array.subtract(index, 1 as $type);
                    }
                    {
                        ftrace::duration!("benchmark", concat!($Array, "::drop/", $size));
                        drop(array);
                    }
               }
            }
        )*
    };
}

macro_rules! bench_arithmetic_array_fns {
    ($name:ident, $type:ty, $Array:expr) => {
        bench_arithmetic_array_fn_impls!($name, $type, $Array, [32, 128, 240]);
    };
}

macro_rules! bench_string_array_fns {
    ([$($size:expr),*]) => {
        $(
            paste::paste! {
                fn [<bench_string_array_ $size>](root: &Node, iteration: usize) {
                    let index = iteration % $size;
                    let array = {
                        ftrace::duration!("benchmark", concat!("Node::create_string_array/", $size));
                        root.create_string_array(NAME, $size)
                    };

                    {
                        ftrace::duration!("benchmark", concat!("StringArrayProperty::set/", $size));
                        array.set(index, "one");
                    }

                    {
                        ftrace::duration!("benchmark", concat!("StringArrayProperty::clear/", $size));
                        array.clear();
                    }

                    {
                        ftrace::duration!("benchmark", concat!("StringArrayProperty::drop/", $size));
                        drop(array);
                    }
                }
            }
        )*
    };
}

fn get_linear_bench_data<T>(size: usize) -> (LinearHistogramParams<T>, T)
where
    T: FromPrimitive + Add<Output = T> + Mul<Output = T> + Copy,
{
    let params = LinearHistogramParams {
        floor: T::from_usize(10).unwrap(),
        step_size: T::from_usize(5).unwrap(),
        buckets: size,
    };
    let x = T::from_usize(size / 2).unwrap();
    let initial_value = params.floor + x * params.step_size;
    (params, initial_value)
}

fn get_exponential_bench_data<T>(size: usize) -> (ExponentialHistogramParams<T>, T)
where
    T: FromPrimitive + Add<Output = T> + Mul<Output = T> + One<Output = T> + Clone + Copy,
{
    let params = ExponentialHistogramParams {
        floor: T::from_usize(10).unwrap(),
        initial_step: T::from_usize(5).unwrap(),
        step_multiplier: T::from_usize(2).unwrap(),
        buckets: size,
    };
    let initial_value = params.floor + params.initial_step * pow(params.step_multiplier, size / 8);
    (params, initial_value)
}

macro_rules! bench_histogram_fn_impls {
    ($name:ident, $type:ty, $Histogram:expr, $histogram_type:ident, [$($size:expr),*]) => {
        $(
            paste::paste! {
                fn [<bench_ $name _ $histogram_type _histogram_ $size>](root: &Node) {
                    let (params, value) = [<get_ $histogram_type _bench_data>]($size);
                    let histogram = {
                        ftrace::duration!(
                            "benchmark",
                            concat!("Node::create_",
                                    stringify!($name), "_", stringify!($histogram_type),
                                    "_histogram/", $size));
                        root.[<create_ $name _ $histogram_type _histogram>](NAME, params)
                    };
                    {
                        ftrace::duration!("benchmark", concat!($Histogram, "::insert/", $size));
                        histogram.insert(value);
                    }
                    {
                        ftrace::duration!("benchmark",
                                          concat!($Histogram, "::insert_overflow/", $size));
                        histogram.insert(10000000 as $type);
                    }
                    {
                        ftrace::duration!("benchmark",
                                          concat!($Histogram, "::insert_underflow/", $size));
                        histogram.insert(0 as $type);
                    }
                    {
                        ftrace::duration!("benchmark",
                                          concat!($Histogram, "::drop/", $size));
                        drop(histogram);
                    }
                }
            }
        )*
    };
}

macro_rules! bench_histogram_fn {
    ($name:ident, $type:ty, $Histogram:expr, $histogram_type:ident) => {
        bench_histogram_fn_impls!($name, $type, $Histogram, $histogram_type, [32, 128, 240]);
    };
}

fn get_string_value(size: usize) -> String {
    std::iter::repeat("a").take(size).collect::<String>()
}

fn get_bytes_value(size: usize) -> Vec<u8> {
    vec![1; size]
}

macro_rules! bench_property_fn_impl {
    ($name:ident, $Property:expr, [$($size:expr),*]) => {
        $(
            paste::paste! {
                fn [<bench_ $name _property_ $size>](root: &Node) {
                    let initial_value = [<get_ $name _value>](0);
                    let value = [<get_ $name _value>]($size);
                    let property = {
                        ftrace::duration!(
                            "benchmark",
                            concat!("Node::create_", stringify!($name), "/", $size));
                        root.[<create_ $name>](NAME, &initial_value)
                    };
                    {
                        ftrace::duration!("benchmark", concat!($Property, "::set/", $size));
                        property.set(&value);
                    }
                    {
                        ftrace::duration!("benchmark",
                                          concat!($Property, "::set_again/", $size));
                        property.set(&value);
                    }
                    {
                        ftrace::duration!("benchmark",
                                          concat!($Property, "::drop/", $size));
                        drop(property);
                    }
                    {
                        ftrace::duration!(
                            "benchmark",
                            concat!("Node::record_", stringify!($name), "/", $size));
                        root.[<record_ $name>](NAME, &initial_value);
                    };
                }
            }
        )*
    };
}

macro_rules! bench_property_fn {
    ($name:ident, $Property:expr) => {
        bench_property_fn_impl!($name, $Property, [4, 8, 100, 2000, 2048, 10000]);
    };
}

bench_numeric_property_fn!(int, i64, "IntProperty");
bench_numeric_property_fn!(uint, u64, "UintProperty");
bench_numeric_property_fn!(double, f64, "DoubleProperty");

bench_arithmetic_array_fns!(int, i64, "IntArrayProperty");
bench_arithmetic_array_fns!(uint, u64, "UintArrayProperty");
bench_arithmetic_array_fns!(double, f64, "DoubleArrayProperty");

bench_string_array_fns!([32, 128, 240]);

bench_histogram_fn!(int, i64, "IntLinearHistogramProperty", linear);
bench_histogram_fn!(uint, u64, "UintLinearHistogramProperty", linear);
bench_histogram_fn!(double, f64, "DoubleLinearHistogramProperty", linear);
bench_histogram_fn!(int, i64, "IntExponentialHistogramProperty", exponential);
bench_histogram_fn!(uint, u64, "UintExponentialHistogramProperty", exponential);
bench_histogram_fn!(double, f64, "DoubleExponentialHistogramProperty", exponential);

bench_property_fn!(string, "StringProperty");
bench_property_fn!(bytes, "BytesProperty");

fn create_inspector() -> Inspector {
    ftrace::duration!("benchmark", "Inspector::new");
    Inspector::new()
}

fn get_root(inspector: &Inspector) -> &Node {
    ftrace::duration!("benchmark", "Inspector::root");
    inspector.root()
}

fn bench_node(root: &Node) {
    let node = {
        ftrace::duration!("benchmark", "Node::create_child");
        root.create_child(NAME)
    };
    {
        ftrace::duration!("benchmark", "Node::drop");
        drop(node);
    }
}

fn bench_heap_extend() {
    let (mapping, _) = {
        ftrace::duration!("benchmark", "Heap::create_1mb_vmo");
        Mapping::allocate(1 << 21)
    }
    .unwrap();
    let mut heap = Heap::new(Arc::new(mapping)).unwrap();
    let mut blocks = Vec::with_capacity(513);
    {
        ftrace::duration!("benchmark", "Heap::allocate_512k");
        for _ in 0..512 {
            blocks.push(heap.allocate_block(2048).unwrap());
        }
    }
    {
        ftrace::duration!("benchmark", "Heap::extend");
        blocks.push(heap.allocate_block(2048).unwrap());
    }
    {
        ftrace::duration!("benchmark", "Heap::free");
        for block in blocks {
            heap.free_block(block).unwrap();
        }
    }
    {
        ftrace::duration!("benchmark", "Heap::drop");
        drop(heap);
    }
}

// Benchmark the write-speed of a local inspector after it has been copy-on-write
// served over FIDL
fn single_iteration_fuchsia_inspect_tree() {
    let inspector = create_inspector();

    let mut nodes = vec![];

    for i in 0..1015 {
        nodes.push(inspector.root().create_int("i", i));
    }

    let proxy = spawn_server(inspector.clone()).unwrap();
    // Force TLB shootdown for following writes on the local inspector
    let _ = proxy.vmo();

    ftrace::duration!("benchmark", "Node::IntProperty::CoW::Add");
    for i in nodes {
        i.add(1);
    }
}

macro_rules! single_iteration_fn {
    (array_sizes: [$($array_size:expr),*], property_sizes: [$($prop_size:expr),*]) => {
        paste::paste! {
            fn single_iteration(iteration: usize) {
                let inspector = create_inspector();
                let root = get_root(&inspector);
                bench_node(&root);
                bench_int_property(&root);
                bench_double_property(&root);
                bench_uint_property(&root);

                $(
                    [<bench_string_property_ $prop_size>](&root);
                    [<bench_bytes_property_ $prop_size>](&root);
                )*
                $(
                    [<bench_int_array_ $array_size>](&root, iteration);
                    [<bench_double_array_ $array_size>](&root, iteration);
                    [<bench_uint_array_ $array_size>](&root, iteration);
                    [<bench_string_array_ $array_size>](&root, iteration);

                    [<bench_int_linear_histogram_ $array_size>](&root);
                    [<bench_uint_linear_histogram_ $array_size>](&root);
                    [<bench_double_linear_histogram_ $array_size>](&root);

                    [<bench_int_exponential_histogram_ $array_size>](&root);
                    [<bench_uint_exponential_histogram_ $array_size>](&root);
                    [<bench_double_exponential_histogram_ $array_size>](&root);
                )*
                bench_heap_extend();
            }
        }
    };
}

single_iteration_fn!(array_sizes: [32, 128, 240], property_sizes: [4, 8, 100, 2000, 2048, 10000]);

enum InspectorState {
    Running,
    Done,
}

/// Start a worker thread that will continually update the given inspector at the given rate.
///
/// Returns a closure that when called cancels the thread, returning only when the thread has
/// exited.
fn start_inspector_update_thread(inspector: Inspector, changes_per_second: usize) -> impl FnOnce() {
    let state = Arc::new(Mutex::new(InspectorState::Running));
    let ret_state = state.clone();

    let child = inspector.root().create_child("test");
    let val = child.create_int("val", 0);

    let thread = std::thread::spawn(move || {
        ftrace::duration!("benchmark", "Thread operation");

        loop {
            {
                ftrace::duration!("benchmark", "Sleep");
                let sleep_time =
                    std::time::Duration::from_nanos(1_000_000_000u64 / changes_per_second as u64);
                std::thread::sleep(sleep_time);
                match *state.lock() {
                    InspectorState::Done => {
                        break;
                    }
                    _ => {}
                }
            }
            {
                ftrace::duration!("benchmark", "Update");
                val.add(1);
            }
        }
    });

    return move || {
        {
            *ret_state.lock() = InspectorState::Done;
        }
        thread.join().expect("join thread");
    };
}

fn add_lazies(inspector: Inspector, num_nodes: usize) {
    let node = inspector.root().create_child("node");

    for i in 0..num_nodes {
        ftrace::duration!("benchmark", "Update");
        node.record_lazy_child(format!("child-{}", i), move || {
            let insp = Inspector::new();
            insp.root().record_int("int", 1);
            async move { Ok(insp) }.boxed()
        });
    }
}

fn spawn_server(inspector: Inspector) -> Result<TreeProxy, Error> {
    let (tree, request_stream) = fidl::endpoints::create_proxy_and_stream::<TreeMarker>()?;
    spawn_tree_server(inspector, TreeServerSettings::default(), request_stream);
    Ok(tree)
}

// Generates a function for benchmarking uncontended reads of a TreeProxy.
macro_rules! reader_tree_uncontended_bench_fn {
    ($name:ident, $size:expr,  $label:expr) => {
        async fn $name(iterations: usize) {
            let inspector = Inspector::new_with_size($size);
            // inspector clones all refer to same VMO
            let proxy = spawn_server(inspector.clone()).unwrap();

            for _ in 0..iterations {
                ftrace::duration!("benchmark", $label);

                loop {
                    ftrace::duration!("benchmark", "TryFromTreeProxy");
                    if let Ok(_) = SnapshotTree::try_from(&proxy).await {
                        break;
                    }
                }
            }
        }
    };
}

// Generates a function for benchmarking contended reads of a TreeProxy.
macro_rules! reader_tree_bench_fn {
    ($name:ident, $size:expr, $freq:expr, $label:expr) => {
        async fn $name(iterations: usize) {
            let inspector = Inspector::new_with_size($size);
            // inspector clones all refer to same VMO
            let proxy = spawn_server(inspector.clone()).unwrap();
            let done_fn = start_inspector_update_thread(inspector.clone(), $freq);
            add_lazies(inspector, 4);

            for _ in 0..iterations {
                ftrace::duration!("benchmark", $label);

                loop {
                    ftrace::duration!("benchmark", "TryFromTreeProxy");
                    if let Ok(_) = SnapshotTree::try_from(&proxy).await {
                        break;
                    }
                }
            }

            done_fn();
        }
    };
}

macro_rules! reader_bench_fn {
    ($name:ident, $size:expr, $freq:expr, $label:expr) => {
        fn $name(iterations: usize) {
            let inspector = Inspector::new_with_size($size);
            let vmo = inspector.duplicate_vmo().expect("failed to duplicate vmo");
            let done_fn = start_inspector_update_thread(inspector, $freq);

            for _ in 0..iterations {
                ftrace::duration!("benchmark", $label);

                loop {
                    ftrace::duration!("benchmark", "TryOnce");
                    if let Ok(_) = Snapshot::try_once_from_vmo(&vmo) {
                        break;
                    }
                }
            }

            done_fn();
        }
    };
}

// Generates a function for benchmarking read of a VMO filled to a specified size.
macro_rules! reader_tree_filled_vmo_bench_fn {
    ($name:ident, $size:expr, $fill_until:expr, $label:expr) => {
        async fn $name(iterations: usize) {
            let inspector = Inspector::new_with_size($size);
            // inspector clones all refer to same VMO
            let proxy = spawn_server(inspector.clone()).unwrap();
            let mut nodes = vec![];
            if $fill_until > 0 {
                let ints_for_filling: i64 = $fill_until / 64;
                for i in 0..ints_for_filling {
                    nodes.push(inspector.root().create_int("i", i));
                }
            }

            for _ in 0..iterations {
                ftrace::duration!("benchmark", $label);

                loop {
                    if let Ok(_) = SnapshotTree::try_from(&proxy).await {
                        break;
                    }
                }
            }
        }
    };
}

reader_tree_uncontended_bench_fn!(uncontended_snapshot_tree_4k, 4096, "UncontendedSnapshotTree/4K");
reader_tree_uncontended_bench_fn!(
    uncontended_snapshot_tree_16k,
    4096 * 4,
    "UncontendedSnapshotTree/16K"
);
reader_tree_uncontended_bench_fn!(
    uncontended_snapshot_tree_256k,
    4096 * 64,
    "UncontendedSnapshotTree/256K"
);
reader_tree_uncontended_bench_fn!(
    uncontended_snapshot_tree_1m,
    4096 * 256,
    "UncontendedSnapshotTree/1M"
);

reader_tree_bench_fn!(snapshot_tree_4k_1hz, 4096, 1, "SnapshotTree/4K/1hz");
reader_tree_bench_fn!(snapshot_tree_4k_10hz, 4096, 10, "SnapshotTree/4K/10hz");
reader_tree_bench_fn!(snapshot_tree_4k_100hz, 4096, 100, "SnapshotTree/4K/100hz");
reader_tree_bench_fn!(snapshot_tree_4k_1khz, 4096, 1000, "SnapshotTree/4K/1khz");
reader_tree_bench_fn!(snapshot_tree_4k_10khz, 4096, 10000, "SnapshotTree/4K/10khz");
reader_tree_bench_fn!(snapshot_tree_4k_100khz, 4096, 100000, "SnapshotTree/4K/100khz");
reader_tree_bench_fn!(snapshot_tree_4k_1mhz, 4096, 1000000, "SnapshotTree/4K/1mhz");
reader_tree_bench_fn!(snapshot_tree_16k_1hz, 4096 * 4, 1, "SnapshotTree/16K/1hz");
reader_tree_bench_fn!(snapshot_tree_16k_10hz, 4096 * 4, 10, "SnapshotTree/16K/10hz");
reader_tree_bench_fn!(snapshot_tree_16k_100hz, 4096 * 4, 100, "SnapshotTree/16K/100hz");
reader_tree_bench_fn!(snapshot_tree_16k_1khz, 4096 * 4, 1000, "SnapshotTree/16K/1khz");
reader_tree_bench_fn!(snapshot_tree_256k_1hz, 4096 * 64, 1, "SnapshotTree/256K/1hz");
reader_tree_bench_fn!(snapshot_tree_256k_10hz, 4096 * 64, 10, "SnapshotTree/256K/10hz");
reader_tree_bench_fn!(snapshot_tree_256k_100hz, 4096 * 64, 100, "SnapshotTree/256K/100hz");
reader_tree_bench_fn!(snapshot_tree_256k_1khz, 4096 * 64, 1000, "SnapshotTree/256K/1khz");
reader_tree_bench_fn!(snapshot_tree_1m_1hz, 4096 * 256, 1, "SnapshotTree/1M/1hz");
reader_tree_bench_fn!(snapshot_tree_1m_10hz, 4096 * 256, 10, "SnapshotTree/1M/10hz");
reader_tree_bench_fn!(snapshot_tree_1m_100hz, 4096 * 256, 100, "SnapshotTree/1M/100hz");
reader_tree_bench_fn!(snapshot_tree_1m_1khz, 4096 * 256, 1000, "SnapshotTree/1M/1khz");

reader_bench_fn!(bench_4k_1hz, 4096, 1, "Snapshot/4K/1hz");
reader_bench_fn!(bench_4k_10hz, 4096, 10, "Snapshot/4K/10hz");
reader_bench_fn!(bench_4k_100hz, 4096, 100, "Snapshot/4K/100hz");
reader_bench_fn!(bench_4k_1khz, 4096, 1000, "Snapshot/4K/1khz");
reader_bench_fn!(bench_4k_10khz, 4096, 10000, "Snapshot/4K/10khz");
reader_bench_fn!(bench_4k_100khz, 4096, 100000, "Snapshot/4K/100khz");
reader_bench_fn!(bench_4k_1mhz, 4096, 1000000, "Snapshot/4K/1mhz");
reader_bench_fn!(bench_256k_1hz, 4096 * 64, 1, "Snapshot/256K/1hz");
reader_bench_fn!(bench_256k_10hz, 4096 * 64, 10, "Snapshot/256K/10hz");
reader_bench_fn!(bench_256k_100hz, 4096 * 64, 100, "Snapshot/256K/100hz");
reader_bench_fn!(bench_256k_1khz, 4096 * 64, 1000, "Snapshot/256K/1khz");
reader_bench_fn!(bench_1m_1hz, 4096 * 256, 1, "Snapshot/1M/1hz");
reader_bench_fn!(bench_1m_10hz, 4096 * 256, 10, "Snapshot/1M/10hz");
reader_bench_fn!(bench_1m_100hz, 4096 * 256, 100, "Snapshot/1M/100hz");
reader_bench_fn!(bench_1m_1khz, 4096 * 256, 1000, "Snapshot/1M/1khz");

reader_tree_filled_vmo_bench_fn!(snapshot_tree_empty_vmo, 4096 * 256, 0, "SnapshotTree/EmptyVMO");
reader_tree_filled_vmo_bench_fn!(
    snapshot_tree_quarter_filled_vmo,
    4096 * 256,
    4096 * 64,
    "SnapshotTree/QuarterFilledVMO"
);
reader_tree_filled_vmo_bench_fn!(
    snapshot_tree_half_filled_vmo,
    4096 * 256,
    4096 * 128,
    "SnapshotTree/HalfFilledVMO"
);
reader_tree_filled_vmo_bench_fn!(
    snapshot_tree_three_quarter_filled_vmo,
    4096 * 256,
    4096 * 192,
    "SnapshotTree/ThreeQuarterFilledVMO"
);
reader_tree_filled_vmo_bench_fn!(
    snapshot_tree_full_vmo,
    4096 * 256,
    4096 * 256,
    "SnapshotTree/FullVMO"
);

async fn reader_benchmark(iterations: usize) {
    // TODO(fxbug.dev/43505): Implement benchmarks where the real size doesn't match the inspector size.
    // TODO(fxbug.dev/43505): Enforce threads starting before benches run.

    bench_4k_1hz(iterations);
    bench_4k_10hz(iterations);
    bench_4k_100hz(iterations);
    bench_4k_1khz(iterations);
    bench_4k_10khz(iterations);
    bench_4k_100khz(iterations);
    bench_4k_1mhz(iterations);
    bench_256k_1hz(iterations);
    bench_256k_10hz(iterations);
    bench_256k_100hz(iterations);
    bench_256k_1khz(iterations);
    bench_1m_1hz(iterations);
    bench_1m_10hz(iterations);
    bench_1m_100hz(iterations);
    bench_1m_1khz(iterations);

    snapshot_tree_4k_1hz(iterations).await;
    snapshot_tree_4k_10hz(iterations).await;
    snapshot_tree_4k_100hz(iterations).await;
    snapshot_tree_4k_1khz(iterations).await;
    snapshot_tree_4k_10khz(iterations).await;
    snapshot_tree_4k_100khz(iterations).await;
    snapshot_tree_4k_1mhz(iterations).await;
    snapshot_tree_16k_1hz(iterations).await;
    snapshot_tree_16k_10hz(iterations).await;
    snapshot_tree_16k_100hz(iterations).await;
    snapshot_tree_16k_1khz(iterations).await;
    snapshot_tree_256k_1hz(iterations).await;
    snapshot_tree_256k_10hz(iterations).await;
    snapshot_tree_256k_100hz(iterations).await;
    snapshot_tree_256k_1khz(iterations).await;
    snapshot_tree_1m_1hz(iterations).await;
    snapshot_tree_1m_10hz(iterations).await;
    snapshot_tree_1m_100hz(iterations).await;
    snapshot_tree_1m_1khz(iterations).await;

    uncontended_snapshot_tree_4k(iterations).await;
    uncontended_snapshot_tree_16k(iterations).await;
    uncontended_snapshot_tree_256k(iterations).await;
    uncontended_snapshot_tree_1m(iterations).await;

    snapshot_tree_empty_vmo(iterations).await;
    snapshot_tree_quarter_filled_vmo(iterations).await;
    snapshot_tree_half_filled_vmo(iterations).await;
    snapshot_tree_three_quarter_filled_vmo(iterations).await;
    snapshot_tree_full_vmo(iterations).await;
}

fn writer_benchmark(iterations: usize) {
    for i in 0..iterations {
        single_iteration(i);
        single_iteration_fuchsia_inspect_tree();
    }
}

#[fuchsia::component]
async fn main() -> Result<(), Error> {
    trace_provider_create_with_fdio();
    let args: Args = argh::from_env();

    match args.benchmark {
        BenchmarkOption::Writer => {
            writer_benchmark(args.iterations);
        }
        BenchmarkOption::Reader => {
            reader_benchmark(args.iterations).await;
        }
    }

    Ok(())
}
