use super::*;

#[repr(align(4096))]
struct TestHeap([u8; 64 * 1024]);

#[test]
fn global_alloc_returns_null_on_oom_and_remains_usable() {
    let mut heap = TestHeap([0; 64 * 1024]);
    // Large page allocator configurations have bitmaps exceeding a test
    // thread's stack. Keep this test's independent allocator in static storage.
    static ALLOCATOR: GlobalAllocator = GlobalAllocator::new();
    let allocator = &ALLOCATOR;
    // Initialize only the byte heap. An empty page allocator makes expansion
    // fail deterministically, without depending on host virtual addresses.
    allocator
        .balloc
        .lock()
        .init(heap.0.as_mut_ptr() as usize, heap.0.len());

    let large = Layout::from_size_align(1024 * 1024, 8).unwrap();
    assert!(allocator.alloc(large).is_err());
    assert!(unsafe { GlobalAlloc::alloc(allocator, large) }.is_null());

    let small = Layout::from_size_align(64, 8).unwrap();
    let ptr = unsafe { GlobalAlloc::alloc(allocator, small) };
    assert!(!ptr.is_null());
    let used = allocator.used_bytes();
    unsafe {
        ptr.write_bytes(0xa5, small.size());
        for i in 0..small.size() {
            assert_eq!(*ptr.add(i), 0xa5);
        }
        GlobalAlloc::dealloc(allocator, ptr, small);
    }
    // Slab may retain a backing page after the user allocation is freed.
    assert_eq!(allocator.used_bytes(), used - small.size());
}

#[test]
fn high_alignment_survives_reallocation_and_oom() {
    const HEAP_SIZE: usize = 4 * 1024 * 1024;

    #[repr(align(1048576))]
    #[allow(dead_code)]
    struct AlignedHeap([u8; HEAP_SIZE]);

    static mut HEAP: AlignedHeap = AlignedHeap([0; HEAP_SIZE]);
    static ALLOCATOR: GlobalAllocator = GlobalAllocator::new();
    let allocator = &ALLOCATOR;
    let start = core::ptr::addr_of_mut!(HEAP).cast::<u8>() as usize;
    allocator.balloc.lock().init(start, HEAP_SIZE);

    for align in [8192, 1024 * 1024] {
        let layout = Layout::from_size_align(64, align).unwrap();
        let before = allocator.used_bytes();
        let ptr = unsafe { GlobalAlloc::alloc(allocator, layout) };
        assert!(!ptr.is_null());
        assert_eq!(ptr as usize % align, 0);
        unsafe { ptr.write_bytes(0xa5, layout.size()) };

        let used = allocator.used_bytes();
        let failed = unsafe { GlobalAlloc::realloc(allocator, ptr, layout, 1 << 30) };
        assert!(failed.is_null());
        assert_eq!(allocator.used_bytes(), used);
        for i in 0..layout.size() {
            assert_eq!(unsafe { *ptr.add(i) }, 0xa5);
        }

        let grown_layout = Layout::from_size_align(128, align).unwrap();
        let grown = unsafe { GlobalAlloc::realloc(allocator, ptr, layout, grown_layout.size()) };
        assert!(!grown.is_null());
        assert_eq!(grown as usize % align, 0);
        for i in 0..layout.size() {
            assert_eq!(unsafe { *grown.add(i) }, 0xa5);
        }
        unsafe { GlobalAlloc::dealloc(allocator, grown, grown_layout) };
        assert_eq!(allocator.used_bytes(), before);

        let zeroed = unsafe { GlobalAlloc::alloc_zeroed(allocator, layout) };
        assert!(!zeroed.is_null());
        assert_eq!(zeroed as usize % align, 0);
        for i in 0..layout.size() {
            assert_eq!(unsafe { *zeroed.add(i) }, 0);
        }
        unsafe { GlobalAlloc::dealloc(allocator, zeroed, layout) };
        assert_eq!(allocator.used_bytes(), before);
    }

    for shift in 13..usize::BITS - 1 {
        let align = 1usize << shift;
        for size in [1, 64, 4096, 4097] {
            let layout = Layout::from_size_align(size, align).unwrap();
            let used = allocator.used_bytes();
            let ptr = unsafe { GlobalAlloc::alloc(allocator, layout) };
            if !ptr.is_null() {
                assert_eq!(ptr as usize % align, 0);
                unsafe { GlobalAlloc::dealloc(allocator, ptr, layout) };
            }
            assert_eq!(allocator.used_bytes(), used);
        }
    }

    let impossible = Layout::from_size_align(64, 1 << 30).unwrap();
    let used = allocator.used_bytes();
    assert!(unsafe { GlobalAlloc::alloc(allocator, impossible) }.is_null());
    assert!(unsafe { GlobalAlloc::alloc_zeroed(allocator, impossible) }.is_null());
    assert_eq!(allocator.used_bytes(), used);
}

#[cfg(feature = "slab")]
#[test]
fn slab_small_objects_keep_their_bucket_and_cached_page_accounting() {
    let mut heap = TestHeap([0; 64 * 1024]);
    let mut allocator = DefaultByteAllocator::new();
    allocator.init(heap.0.as_mut_ptr() as usize, heap.0.len());

    let small = Layout::from_size_align(1, 8).unwrap();
    let same_bucket = Layout::from_size_align(63, 8).unwrap();
    let first = allocator.alloc(small).unwrap();
    let first_used = allocator.used_bytes();
    let second = allocator.alloc(same_bucket).unwrap();
    assert_eq!(allocator.used_bytes() - first_used, 64);
    allocator.dealloc(first, small);
    allocator.dealloc(second, same_bucket);
    assert_eq!(first_used - allocator.used_bytes(), 64);
    // A cached backing page is still charged by the upstream slab allocator.
    assert!(allocator.used_bytes() > 0);

    let impossible = Layout::from_size_align(0, 1 << (usize::BITS - 1)).unwrap();
    assert!(matches!(
        allocator.alloc(impossible),
        Err(axallocator::AllocError::InvalidParam)
    ));
}

#[cfg(feature = "slab")]
#[test]
fn slab_high_alignment_uses_added_memory_and_releases_its_backing_block() {
    const ALIGN: usize = 1024 * 1024;

    #[repr(align(1048576))]
    #[allow(dead_code)]
    struct AlignedHeap([u8; 2 * ALIGN]);

    static mut HEAP: AlignedHeap = AlignedHeap([0; 2 * ALIGN]);
    let start = core::ptr::addr_of_mut!(HEAP).cast::<u8>() as usize;
    let mut allocator = DefaultByteAllocator::new();
    allocator.init(start, 64 * 1024);
    allocator.add_memory(start + ALIGN, ALIGN).unwrap();

    let layout = Layout::from_size_align(64, ALIGN).unwrap();
    let available = allocator.available_bytes();
    let ptr = allocator.alloc(layout).unwrap();
    assert_eq!(ptr.as_ptr() as usize, start + ALIGN);
    assert_eq!(allocator.used_bytes(), ALIGN);
    unsafe { ptr.as_ptr().write_bytes(0x5a, layout.size()) };
    allocator.dealloc(ptr, layout);
    assert_eq!(allocator.used_bytes(), 0);
    assert_eq!(allocator.available_bytes(), available);
}
