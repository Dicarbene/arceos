use axallocator::{AllocError, AllocResult, BaseAllocator, ByteAllocator};
use core::alloc::Layout;
use core::ptr::NonNull;

const MAX_SLAB_SIZE: usize = 4096;

/// A slab allocator that preserves alignments larger than its largest slab.
pub struct SlabByteAllocator {
    inner: axallocator::SlabByteAllocator,
}

impl SlabByteAllocator {
    /// Creates an empty allocator.
    pub const fn new() -> Self {
        Self {
            inner: axallocator::SlabByteAllocator::new(),
        }
    }

    fn backing_layout(layout: Layout) -> AllocResult<Layout> {
        if layout.size() <= MAX_SLAB_SIZE && layout.align() > MAX_SLAB_SIZE {
            // The upstream slab allocator only selects its buddy allocator by
            // size. Reserve an alignment-sized block to select that path.
            Layout::from_size_align(layout.align(), layout.align())
                .map_err(|_| AllocError::InvalidParam)
        } else {
            Ok(layout)
        }
    }
}

impl Default for SlabByteAllocator {
    fn default() -> Self {
        Self::new()
    }
}

impl BaseAllocator for SlabByteAllocator {
    fn init(&mut self, start: usize, size: usize) {
        self.inner.init(start, size);
    }

    fn add_memory(&mut self, start: usize, size: usize) -> AllocResult {
        self.inner.add_memory(start, size)
    }
}

impl ByteAllocator for SlabByteAllocator {
    fn alloc(&mut self, layout: Layout) -> AllocResult<NonNull<u8>> {
        self.inner.alloc(Self::backing_layout(layout)?)
    }

    fn dealloc(&mut self, pos: NonNull<u8>, layout: Layout) {
        let layout = Self::backing_layout(layout).expect("layout of a successful slab allocation");
        self.inner.dealloc(pos, layout);
    }

    fn total_bytes(&self) -> usize {
        self.inner.total_bytes()
    }

    fn used_bytes(&self) -> usize {
        self.inner.used_bytes()
    }

    fn available_bytes(&self) -> usize {
        self.inner.available_bytes()
    }
}
