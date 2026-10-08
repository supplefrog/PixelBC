# Bounded retained-block transfers in PixelBC 0.1.2

This extension preserves the original compressed bytes for accepted optimal 2D
BC images created with `TRANSFER_SRC` usage. Each such image owns a device buffer
and its memory. Mip-major storage contains tight block rows for every array layer,
including complete blocks for small edge mips. Checked arithmetic rejects size
overflow. Upload-only images allocate no compressed store and perform no extra
compressed copy; the existing NFS sampled/upload path retains its allocation and
GPU decode mechanism.

Buffer uploads update both the canonical compressed store and the decoded RGBA
image. Image-to-buffer commands copy the original blocks with the requested
offset, row pitch and layer pitch. They leave buffer padding untouched and never
reconstruct compressed bytes from RGBA. Image copies between the same BC family,
including its UNORM/SRGB pair, preserve canonical blocks and decoded RGBA bytes.
A destination without `TRANSFER_SRC` needs only the RGBA copy. The transfer
implementation uses the format helpers, so the separate BC7 decoder integration
also receives the same retained-byte behavior; BC6 remains explicitly unsupported.

Caller image barriers are mirrored into the matching canonical mip/layer buffer
ranges. Original stage/access masks, dependency flags and queue-family indices
are preserved. This covers legacy and synchronization2 pipeline barriers, legacy
and synchronization2 event waits, synchronization2 event signals and KHR aliases.
Canonical buffer operations execute in COPY/TRANSFER, so native semaphore waits
at those stages cover both stored blocks and image transfers. Submissions,
semaphores and fences remain native; no extra queue submission or CPU map is
inserted. Internal compute state restoration follows the existing decoder contract.

Command buffers retain their referenced stores until reset/free, including after
a completed source image is destroyed. Destroying a source image can invalidate
the native executable command buffer under Vulkan rules: callers must reset it
before recording or replaying subsequent work. Tests destroy the source only
after fence completion and then reset before destination readback. Native
allocation failures and host ownership failures roll back image/buffer/memory
handles. Defensive device cleanup drops private command/store ownership before
native device destruction; it does not relax caller object-lifetime rules.

The additive `pvb_get_transfer_stats` export preserves the original `PvbStats`
ABI. It reports live store count, logical compressed content bytes and recorded
canonical-copy bytes. Native allocations can include alignment padding beyond
the logical byte count. Recorded copy bytes describe command recording, not
physical execution count, GPU bandwidth or performance. One accepted and one
unsupported creation request per BC format/process log format ID, extent, mips,
layers, usage and creation shape through `PixelBC`; no asset names or bytes are
logged. Image creation always precedes the accepted log.

Remaining bounds: primary command buffers, optimal 2D single-sample images,
sampled/transfer usages, supported block-aligned regions with edge exceptions,
and compatible decode families. BC/uncompressed block reinterpretation and copies
between different BC decode families are valid Vulkan operations in some cases
but deliberately fail in this subset. Secondary BC commands, BC storage/linear/
3D/protected/external/sparse images, unsupported creation/copy/barrier extension
chains and BC6 fail explicitly. The global core BC feature remains false.

## Exact upstream path evidence

Read-only Unleashed 0.6.0 revision
`f9ad6baffe32637ff45f34825c247513e8b379d7`:

- `thirdparty/plume/plume_vulkan.cpp:1012` creates textures with
  `SAMPLED|TRANSFER_DST|TRANSFER_SRC`, with optional render/storage flags.
- Its `copyTextureRegion` at 3324 uploads a placed buffer footprint into an image,
  otherwise issuing `vkCmdCopyImage` at 3386. `copyTexture` at 3406 issues batched
  `vkCmdCopyImage` at 3438. Plume contains no `vkCmdCopyImageToBuffer` call.
- All concrete `copyTextureRegion` calls in `UnleashedRecomp/gpu/video.cpp` use
  buffer sources: ImGui font at 1491, mapped texture unlock at 2384, R8 fill at 5558, RGBA
  volume fills at 5652/5656, DDS mip/array/cube upload at 6331 (face duplication at 6345),
  and stb RGBA upload at 6397. DDS uploads preserve padded block rows at 6321–6345.
- There are no `copyTexture` callers in `UnleashedRecomp`. Surface MSAA resolves
  occur at 3672/3674 and cannot apply to accepted single-sample BC images. Plume
  barriers at 2920 onward use full image mip/layer ranges with native legacy
  image barriers and generic memory-read/write access masks.

This establishes source paths and bounds. Actual game/mod format inventory,
runtime calls, WSI, Mali synchronization and performance require local evidence.

## Validation

See [VALIDATION.md](VALIDATION.md) for the current 0.1.2 evidence and limitations.
