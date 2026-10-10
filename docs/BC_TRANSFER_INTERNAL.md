# BC transfer behavior

The upload implementation preserves the caller's command order: source-buffer GPU copy, shader conversion, output-buffer to native image copy, with internal transfer/compute barriers. It does not read arbitrary GPU upload buffers on the CPU or add queue submissions/waits. Invalid regions and unsupported usage fail through the existing diagnostic/error path rather than silently dropping work.

For supported virtual images with transfer-source usage, retained original BC storage is independent of RGBA/ETC2 sampled backing. Supported readbacks reproduce the compressed bytes, including supported offsets/row layouts/mips/layers. A same-family image copy uses original BC bytes and regenerates the destination backing when required, so a lossless source and ETC2 destination do not copy incompatible native pixels. Unsupported mixed families/uncompressed copies and secondary-command-buffer BC operations remain outside the contract.

Resources and descriptors belong to a command recording and survive execution/replay. Explicit/implicit command resets, pool resets, free and device teardown retire them under the retained ownership rules; failed allocation/recording paths are covered by the unchanged fault fixtures. Application synchronization and queue ownership are not bypassed.

The exact 0.1.9 shape policy is selected at creation and shared with supported core/KHR memory queries; a partial upload cannot change the backing. Image-view restrictions and sRGB interpretation follow the virtual format family. Consult the executable tests for precise accepted regions and negative paths, and [VALIDATION.md](VALIDATION.md) for evidentiary limits.
