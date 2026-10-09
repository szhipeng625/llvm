# adviskv vcpkg binary cache

恢复方法（在仓库根目录执行）：
cat deps-cache/binary-cache.tar.gz.part-* | tar xzf - -C third_party/adviskv/.adviskv_deps/vcpkg

恢复后再运行 third_party/adviskv/scripts/setup.sh 会直接命中缓存，无需重新编译依赖。
生成于 kimi（20核/x64-linux, VCPKG_BUILD_TYPE=release, vcpkg 2026-03-04）。
