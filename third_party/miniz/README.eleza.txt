miniz 3.0.2 — https://github.com/richgel999/miniz/releases/tag/3.0.2

miniz.c, miniz.h and LICENSE are taken unchanged from miniz-3.0.2.zip of that
release (sha256 ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5).

To update: replace the three files with the ones of a newer release archive,
change the version here and in third_party/CMakeLists.txt, and run the tests
(tests/core/test_updater.cpp unpacks hostile archives through it).
