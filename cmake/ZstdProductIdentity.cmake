# Exact preceding generated revisions admitted only for product-identity
# migration.
set(_zstd_identity_0_path "lib/dictBuilder/divsufsort.c")
set(_zstd_identity_0_old
    "55e83c7ec4221638a9f5333893ed44441d9c323e9567e68d03ed9a24341bbfad")
set(_zstd_identity_0_new
    "5a1a7a1ab14e4d930d586bf84ca0d015180df197abe2690767fce6712cb77d35")
set(_zstd_identity_1_path "lib/legacy/zstd_legacy.h")
set(_zstd_identity_1_old
    "f58b297da5abbf80e1e3185a8b28f5182d48687e6eaa23043e488608a0ad7a10")
set(_zstd_identity_1_new
    "91a68c85c18479023cdb635e1bf704cdb65719e4bb14748e14a8c1a1b5e25260")
set(_zstd_identity_2_path "lib/legacy/zstd_v05.c")
set(_zstd_identity_2_old
    "e3d1bd2748d27495c03315c7fc4af91f55d3d7bde69d034b16fe0d433c68854e")
set(_zstd_identity_2_new
    "1acf1966ee63e81576c91eb915269e043cfe1fdfae98fa506fddb3e106b3eac6")
set(_zstd_identity_3_path "lib/legacy/zstd_v06.c")
set(_zstd_identity_3_old
    "3252ce5e8e40164860e99417dfd84864683326b24759564d9f678bc77bc2d08e")
set(_zstd_identity_3_new
    "dfaa535a3034a9703d5253db67cb24b0b88add6a92e427862b2a0eb38a9e25ee")
set(_zstd_identity_4_path "lib/legacy/zstd_v07.c")
set(_zstd_identity_4_old
    "6a62f8362843690f3f4df5ca0a220bbe4d161be654bc3ebba80065770b9718b9")
set(_zstd_identity_4_new
    "703b0cba069d4500cdd7477f4698efbb9f8a14d3629d1a069ccbc7d9329b9628")

# Purpose: Migrate exact cached generated files when private product names
# change. Inputs: A pinned Zstandard source tree and complete old/new file
# identities. Outputs: Atomically verified renamed sources; unknown bytes remain
# subject to the normal pipeline's rejection. This never modifies upstream
# provenance.
function(flitzzip_migrate_zstd_product_identity source_dir)
  foreach(index RANGE 0 4 1)
    set(key "_zstd_identity_${index}")
    set(source "${source_dir}/${${key}_path}")
    file(SHA256 "${source}" actual)
    if(NOT actual STREQUAL "${${key}_old}")
      continue()
    endif()
    file(READ "${source}" content)
    string(REPLACE "SuperZip" "FlitzZip" content "${content}")
    string(REPLACE "superzip" "flitzzip" content "${content}")
    string(REPLACE "SUPERZIP" "FLITZZIP" content "${content}")
    flitzzip_write_verified_zstd_patch("${source}" "${actual}" "${${key}_new}"
                                       "${content}")
  endforeach()
endfunction()
