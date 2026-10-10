# Bounded fitting and specialization

The altered GPU adaptation uses Basis Universal2.5 mean/orientation and four-table range-fit mechanisms from `transcoder/basisu_transcoder.cpp`, pinned to `99f52d63aa6799cbdaecfe977111dc5ec3b31d47`. It operates on weighted BC palettes and constrains source alpha coverage. It includes bounded recovery rather than assuming a common fit is always legal or meets the quality guard. It does not embed the Basis library, lookup tables or CPU transcoder.

The production BC1 module excludes BC3 alpha/recovery code and retains the weighted RGB fitting path. The production BC3 specialization retains the corrected alpha route. This source export changes neither shader. A fast fitting alternative and punchthrough-storage alternative are not selected. The default shape policy instead uses the established lossless route for two exact BC1_RGBA shapes.

Adapted Basis mechanisms carry the Binomial LLC copyright and Apache-2.0 notice in the source and [THIRD_PARTY.md](THIRD_PARTY.md); the complete license is in [Apache-2.0.txt](../licenses/Apache-2.0.txt). Independent original-BC/detex expectations remain separate from the production encoder. Quality limits and known failed evidence are in [VALIDATION.md](VALIDATION.md).
