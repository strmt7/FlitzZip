# Reviewed comments preserve compiled tokens and upstream license notices.
# Purpose: Supply exact source identities and descriptive comments. Inputs: The
# pinned dependency patch. Outputs: Local immutable patch data.
set(_zstd_comment_file_count 17)
set(_zstd_comment_0_path "lib/common/entropy_common.c")
set(_zstd_comment_0_original
    "7dfce29c6bc807645b1f0c373dad94bbb49603b175a84e72d1739bf9df65feb8")
set(_zstd_comment_0_patched
    "b191e101752c599c47c57c70afb1dad55742df76c7c3273ddd13b32095d56a5f")
set(_zstd_comment_0_prior
    "b191e101752c599c47c57c70afb1dad55742df76c7c3273ddd13b32095d56a5f")
set(_zstd_comment_0_prior_count 1)
set(_zstd_comment_0_count 1)
# Alerts: 1543.
string(
  CONCAT _zstd_comment_0_0_old
         [=[/* ZSTD_memset(huffWeight, 0, hwSize);   *//* is n]=]
         [=[ot necessary, even though some analyzer complain .]=] [=[.. */]=])
string(CONCAT _zstd_comment_0_0_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
set(_zstd_comment_1_path "lib/common/huf.h")
set(_zstd_comment_1_original
    "9a83d899c8c9bf03389d482562090ec2443bc0f87f27864a2b891b180333f2d4")
set(_zstd_comment_1_patched
    "bf37cde32f78c70774fd1f3137ef744ca553c667f861ad22e511ff737fa3f9a1")
set(_zstd_comment_1_prior
    "bf37cde32f78c70774fd1f3137ef744ca553c667f861ad22e511ff737fa3f9a1")
set(_zstd_comment_1_prior_count 1)
set(_zstd_comment_1_count 1)
# Alerts: 1542.
string(CONCAT _zstd_comment_1_0_old [=[/* no final ; */]=])
string(CONCAT _zstd_comment_1_0_new
              [=[/* The caller supplies the trailing semicolon. */]=])
set(_zstd_comment_2_path "lib/common/xxhash.h")
set(_zstd_comment_2_original
    "8cb837b21a8fe9a6b9dbcd0961ab16e733bfcbfa9e003f3a496ce07ae80aa8ee")
set(_zstd_comment_2_patched
    "327a38e032aa6daccd0145fce664d6c65d001290955fd9c9d47aedd6fcc9cb41")
set(_zstd_comment_2_prior
    "327a38e032aa6daccd0145fce664d6c65d001290955fd9c9d47aedd6fcc9cb41")
set(_zstd_comment_2_prior_count 55)
set(_zstd_comment_2_count 55)
# Alerts: 1598.
string(CONCAT _zstd_comment_2_0_old
              [=[/* #define instead of static const, to be used as ]=]
              [=[initializers */]=])
string(CONCAT _zstd_comment_2_0_new
              [=[/* Macro constants also support compile-time initi]=]
              [=[alizers. */]=])
# Alerts: 1597.
string(CONCAT _zstd_comment_2_1_old
              [=[/* #define rather that static const, to be used as]=]
              [=[ initializers */]=])
string(CONCAT _zstd_comment_2_1_new
              [=[/* Macro constants also support compile-time initi]=]
              [=[alizers. */]=])
# Alerts: 1596.
string(CONCAT _zstd_comment_2_2_old [=[/* data_vec    = input[0]; */]=])
string(CONCAT _zstd_comment_2_2_new [=[/* Load the current input vector. */]=])
# Alerts: 1595.
string(CONCAT _zstd_comment_2_3_old [=[/* key_vec     = secret[0]; */]=])
string(CONCAT _zstd_comment_2_3_new
              [=[/* Load the corresponding secret vector. */]=])
# Alerts: 1594.
string(CONCAT _zstd_comment_2_4_old
              [=[/* data_key    = data_vec ^ key_vec; */]=])
string(CONCAT _zstd_comment_2_4_new
              [=[/* Combine input and secret lanes with exclusive-o]=]
              [=[r. */]=])
# Alerts: 1593.
string(CONCAT _zstd_comment_2_5_old [=[/* data_key_lo = data_key >> 32; */]=])
string(CONCAT _zstd_comment_2_5_new
              [=[/* Select the high 32 bits of each combined input ]=]
              [=[lane. */]=])
# Alerts: 1592.
string(CONCAT _zstd_comment_2_6_old
              [=[/* product     = (data_key & 0xffffffff) * (data_k]=]
              [=[ey_lo & 0xffffffff); */]=])
string(CONCAT _zstd_comment_2_6_new
              [=[/* Multiply the low and high 32-bit halves of each]=]
              [=[ combined lane. */]=])
# Alerts: 1591.
string(CONCAT _zstd_comment_2_7_old [=[/* xacc[0] += swap(data_vec); */]=])
string(CONCAT _zstd_comment_2_7_new
              [=[/* Add input lanes with their paired halves exchan]=]
              [=[ged to the accumulator. */]=])
# Alerts: 1590.
string(CONCAT _zstd_comment_2_8_old [=[/* xacc[0] += product; */]=])
string(CONCAT _zstd_comment_2_8_new
              [=[/* Add the lane products to the accumulator. */]=])
# Alerts: 1589.
string(CONCAT _zstd_comment_2_9_old [=[/* xacc[0] ^= secret; */]=])
string(CONCAT _zstd_comment_2_9_new
              [=[/* Combine the accumulator with the secret lanes u]=]
              [=[sing exclusive-or. */]=])
# Alerts: 1588.
string(CONCAT _zstd_comment_2_10_old [=[/* xacc[0] *= XXH_PRIME32_1; */]=])
string(CONCAT _zstd_comment_2_10_new
              [=[/* Multiply each accumulator lane by the 32-bit pr]=]
              [=[ime. */]=])
# Alerts: 1587.
string(CONCAT _zstd_comment_2_11_old [=[/* data_vec    = xinput[i]; */]=])
string(CONCAT _zstd_comment_2_11_new [=[/* Load the current input vector. */]=])
# Alerts: 1586.
string(CONCAT _zstd_comment_2_12_old [=[/* key_vec     = xsecret[i]; */]=])
string(CONCAT _zstd_comment_2_12_new
              [=[/* Load the corresponding secret vector. */]=])
# Alerts: 1585.
string(CONCAT _zstd_comment_2_13_old
              [=[/* data_key    = data_vec ^ key_vec; */]=])
string(CONCAT _zstd_comment_2_13_new
              [=[/* Combine input and secret lanes with exclusive-o]=]
              [=[r. */]=])
# Alerts: 1584.
string(CONCAT _zstd_comment_2_14_old [=[/* data_key_lo = data_key >> 32; */]=])
string(CONCAT _zstd_comment_2_14_new
              [=[/* Select the high 32 bits of each combined input ]=]
              [=[lane. */]=])
# Alerts: 1583.
string(CONCAT _zstd_comment_2_15_old
              [=[/* product     = (data_key & 0xffffffff) * (data_k]=]
              [=[ey_lo & 0xffffffff); */]=])
string(CONCAT _zstd_comment_2_15_new
              [=[/* Multiply the low and high 32-bit halves of each]=]
              [=[ combined lane. */]=])
# Alerts: 1582.
string(CONCAT _zstd_comment_2_16_old [=[/* xacc[i] += swap(data_vec); */]=])
string(CONCAT _zstd_comment_2_16_new
              [=[/* Add input lanes with their paired halves exchan]=]
              [=[ged to the accumulator. */]=])
# Alerts: 1581.
string(CONCAT _zstd_comment_2_17_old [=[/* xacc[i] += product; */]=])
string(CONCAT _zstd_comment_2_17_new
              [=[/* Add the lane products to the accumulator. */]=])
# Alerts: 1580.
string(CONCAT _zstd_comment_2_18_old [=[/* xacc[i] ^= xsecret; */]=])
string(CONCAT _zstd_comment_2_18_new
              [=[/* Combine the accumulator with the secret lanes u]=]
              [=[sing exclusive-or. */]=])
# Alerts: 1579.
string(CONCAT _zstd_comment_2_19_old [=[/* xacc[i] *= XXH_PRIME32_1; */]=])
string(CONCAT _zstd_comment_2_19_new
              [=[/* Multiply each accumulator lane by the 32-bit pr]=]
              [=[ime. */]=])
# Alerts: 1578.
string(CONCAT _zstd_comment_2_20_old [=[/* data_vec    = xinput[i]; */]=])
string(CONCAT _zstd_comment_2_20_new [=[/* Load the current input vector. */]=])
# Alerts: 1577.
string(CONCAT _zstd_comment_2_21_old [=[/* key_vec     = xsecret[i]; */]=])
string(CONCAT _zstd_comment_2_21_new
              [=[/* Load the corresponding secret vector. */]=])
# Alerts: 1576.
string(CONCAT _zstd_comment_2_22_old
              [=[/* data_key    = data_vec ^ key_vec; */]=])
string(CONCAT _zstd_comment_2_22_new
              [=[/* Combine input and secret lanes with exclusive-o]=]
              [=[r. */]=])
# Alerts: 1575.
string(CONCAT _zstd_comment_2_23_old [=[/* data_key_lo = data_key >> 32; */]=])
string(CONCAT _zstd_comment_2_23_new
              [=[/* Select the high 32 bits of each combined input ]=]
              [=[lane. */]=])
# Alerts: 1574.
string(CONCAT _zstd_comment_2_24_old
              [=[/* product     = (data_key & 0xffffffff) * (data_k]=]
              [=[ey_lo & 0xffffffff); */]=])
string(CONCAT _zstd_comment_2_24_new
              [=[/* Multiply the low and high 32-bit halves of each]=]
              [=[ combined lane. */]=])
# Alerts: 1573.
string(CONCAT _zstd_comment_2_25_old [=[/* xacc[i] += swap(data_vec); */]=])
string(CONCAT _zstd_comment_2_25_new
              [=[/* Add input lanes with their paired halves exchan]=]
              [=[ged to the accumulator. */]=])
# Alerts: 1572.
string(CONCAT _zstd_comment_2_26_old [=[/* xacc[i] += product; */]=])
string(CONCAT _zstd_comment_2_26_new
              [=[/* Add the lane products to the accumulator. */]=])
# Alerts: 1571.
string(CONCAT _zstd_comment_2_27_old [=[/* xacc[i] ^= xsecret[i]; */]=])
string(CONCAT _zstd_comment_2_27_new
              [=[/* Combine the accumulator with the secret lanes u]=]
              [=[sing exclusive-or. */]=])
# Alerts: 1570.
string(CONCAT _zstd_comment_2_28_old [=[/* xacc[i] *= XXH_PRIME32_1; */]=])
string(CONCAT _zstd_comment_2_28_new
              [=[/* Multiply each accumulator lane by the 32-bit pr]=]
              [=[ime. */]=])
# Alerts: 1569.
string(CONCAT _zstd_comment_2_29_old [=[/* data_vec = xinput[i]; */]=])
string(CONCAT _zstd_comment_2_29_new [=[/* Load the current input vector. */]=])
# Alerts: 1568.
string(CONCAT _zstd_comment_2_30_old [=[/* key_vec  = xsecret[i];  */]=])
string(CONCAT _zstd_comment_2_30_new
              [=[/* Load the corresponding secret vector. */]=])
# Alerts: 1567.
string(CONCAT _zstd_comment_2_31_old [=[/* data_key = data_vec ^ key_vec; */]=])
string(CONCAT _zstd_comment_2_31_new
              [=[/* Combine input and secret lanes with exclusive-o]=]
              [=[r. */]=])
# Alerts: 1566.
string(CONCAT _zstd_comment_2_32_old [=[/* xacc[i] = acc_vec + sum; */]=])
string(CONCAT _zstd_comment_2_32_new
              [=[/* Add the computed lane sums to the accumulator. ]=] [=[*/]=])
# Alerts: 1565.
string(CONCAT _zstd_comment_2_33_old [=[/* data_vec = xinput[i]; */]=])
string(CONCAT _zstd_comment_2_33_new [=[/* Load the current input vector. */]=])
# Alerts: 1564.
string(CONCAT _zstd_comment_2_34_old [=[/* key_vec  = xsecret[i];  */]=])
string(CONCAT _zstd_comment_2_34_new
              [=[/* Load the corresponding secret vector. */]=])
# Alerts: 1563.
string(CONCAT _zstd_comment_2_35_old [=[/* data_key = data_vec ^ key_vec; */]=])
string(CONCAT _zstd_comment_2_35_new
              [=[/* Combine input and secret lanes with exclusive-o]=]
              [=[r. */]=])
# Alerts: 1562.
string(CONCAT _zstd_comment_2_36_old
              [=[/* data_key_lo = data_key & 0xFFFFFFFF; */]=])
string(CONCAT _zstd_comment_2_36_new
              [=[/* Select the low 32 bits of each combined input l]=]
              [=[ane. */]=])
# Alerts: 1561.
string(CONCAT _zstd_comment_2_37_old [=[/* data_key_hi = data_key >> 32; */]=])
string(CONCAT _zstd_comment_2_37_new
              [=[/* Select the high 32 bits of each combined input ]=]
              [=[lane. */]=])
# Alerts: 1560.
string(CONCAT _zstd_comment_2_38_old
              [=[/* sum = data_swap + (u64x2) data_key_lo * (u64x2)]=]
              [=[ data_key_hi; */]=])
string(CONCAT _zstd_comment_2_38_new
              [=[/* Add swapped input lanes to products of their lo]=]
              [=[w and high halves. */]=])
# Alerts: 1559.
string(CONCAT _zstd_comment_2_39_old [=[/* xacc[i] = acc_vec + sum; */]=])
string(CONCAT _zstd_comment_2_39_new
              [=[/* Add the computed lane sums to the accumulator. ]=] [=[*/]=])
# Alerts: 1558.
string(CONCAT _zstd_comment_2_40_old [=[/* xacc[i] ^= (xacc[i] >> 47); */]=])
string(CONCAT _zstd_comment_2_40_new
              [=[/* Mix each accumulator lane with its right-shifte]=]
              [=[d value. */]=])
# Alerts: 1557.
string(CONCAT _zstd_comment_2_41_old [=[/* xacc[i] ^= xsecret[i]; */]=])
string(CONCAT _zstd_comment_2_41_new
              [=[/* Combine the accumulator with the secret lanes u]=]
              [=[sing exclusive-or. */]=])
# Alerts: 1556.
string(CONCAT _zstd_comment_2_42_old
              [=[/* xacc[i] = prod_hi + lo(data_key) * XXH_PRIME32_]=]
              [=[1; */]=])
string(CONCAT _zstd_comment_2_42_new
              [=[/* Combine high-half products with the low-half pr]=]
              [=[ime products. */]=])
# Alerts: 1555.
string(CONCAT _zstd_comment_2_43_old [=[/* data_vec = xinput[i]; */]=])
string(CONCAT _zstd_comment_2_43_new [=[/* Load the current input vector. */]=])
# Alerts: 1554.
string(CONCAT _zstd_comment_2_44_old [=[/* key_vec = xsecret[i]; */]=])
string(CONCAT _zstd_comment_2_44_new
              [=[/* Load the corresponding secret vector. */]=])
# Alerts: 1553.
string(CONCAT _zstd_comment_2_45_old
              [=[/* shuffled = (data_key << 32) | (data_key >> 32);]=]
              [=[ */]=])
string(CONCAT _zstd_comment_2_45_new
              [=[/* Exchange the high and low 32-bit halves within ]=]
              [=[each lane. */]=])
# Alerts: 1552.
string(CONCAT _zstd_comment_2_46_old
              [=[/* product = ((xxh_u64x2)data_key & 0xFFFFFFFF) * ]=]
              [=[((xxh_u64x2)shuffled & 0xFFFFFFFF); */]=])
string(CONCAT _zstd_comment_2_46_new
              [=[/* Multiply the low and high 32-bit halves of each]=]
              [=[ combined lane. */]=])
# Alerts: 1551.
string(CONCAT _zstd_comment_2_47_old [=[/* acc_vec = xacc[i]; */]=])
string(CONCAT _zstd_comment_2_47_new
              [=[/* Load the current accumulator vector. */]=])
# Alerts: 1550.
string(CONCAT _zstd_comment_2_48_old [=[/* xacc[i] ^= (xacc[i] >> 47); */]=])
string(CONCAT _zstd_comment_2_48_new
              [=[/* Mix each accumulator lane with its right-shifte]=]
              [=[d value. */]=])
# Alerts: 1549.
string(CONCAT _zstd_comment_2_49_old [=[/* xacc[i] ^= xsecret[i]; */]=])
string(CONCAT _zstd_comment_2_49_new
              [=[/* Combine the accumulator with the secret lanes u]=]
              [=[sing exclusive-or. */]=])
# Alerts: 1548.
string(CONCAT _zstd_comment_2_50_old
              [=[/* prod_lo = ((xxh_u64x2)data_key & 0xFFFFFFFF) * ]=]
              [=[((xxh_u64x2)prime & 0xFFFFFFFF);  */]=])
string(CONCAT _zstd_comment_2_50_new
              [=[/* Multiply the low 32-bit halves of the combined ]=]
              [=[lanes and prime. */]=])
# Alerts: 1547.
string(CONCAT _zstd_comment_2_51_old
              [=[/* prod_hi = ((xxh_u64x2)data_key >> 32) * ((xxh_u]=]
              [=[64x2)prime >> 32);  */]=])
string(CONCAT _zstd_comment_2_51_new
              [=[/* Multiply the high 32-bit halves of the combined]=]
              [=[ lanes and prime. */]=])
# Alerts: 1546.
string(CONCAT _zstd_comment_2_52_old
              [=[/* svprfd(svbool_t, void *, enum svfprop); */]=])
string(CONCAT _zstd_comment_2_52_new
              [=[/* Prefetch the next input region for streaming re]=]
              [=[ads. */]=])
# Alerts: 1545.
string(CONCAT _zstd_comment_2_53_old
              [=[/* m128 ^= XXH_swap64(m128 >> 64); */]=])
string(CONCAT _zstd_comment_2_53_new
              [=[/* Mix the byte-swapped high half into the low hal]=]
              [=[f. */]=])
# Alerts: 1544.
string(CONCAT _zstd_comment_2_54_old
              [=[/* 128x64 multiply: h128 = m128 * XXH_PRIME64_2; *]=] [=[/]=])
string(CONCAT _zstd_comment_2_54_new
              [=[/* Multiply the 128-bit accumulator by the 64-bit ]=]
              [=[prime. */]=])
set(_zstd_comment_3_path "lib/compress/fse_compress.c")
set(_zstd_comment_3_original
    "5070807a489757b87c1e1f50332b099e8ffe22971228218111eef64d3321ae82")
set(_zstd_comment_3_patched
    "6dfd0803dcb03b6ce85fc180cef7ea306fe2eb91fa97e01f2511775fbbe733ea")
set(_zstd_comment_3_prior
    "6dfd0803dcb03b6ce85fc180cef7ea306fe2eb91fa97e01f2511775fbbe733ea")
set(_zstd_comment_3_prior_count 1)
set(_zstd_comment_3_count 1)
# Alerts: 1599.
string(
  CONCAT _zstd_comment_3_0_old
         [=[/* all values are pretty poor;
           probably]=]
         [=[ incompressible data (should have already been det]=]
         [=[ected);
           find max, then give all remaini]=]
         [=[ng points to max */]=])
string(
  CONCAT _zstd_comment_3_0_new
         [=[/* Nearly uniform frequencies indicate incompressi]=]
         [=[ble data.
           Assign remaining normalizatio]=]
         [=[n points to the most frequent symbol
           af]=]
         [=[ter choosing it from the observed counts. */]=])
set(_zstd_comment_4_path "lib/compress/zstd_compress.c")
set(_zstd_comment_4_original
    "10c315ae609d49b2fc431521aa95908086e69fd917991c5ed9a875af3d1208e8")
set(_zstd_comment_4_patched
    "ac0cf6cae090170d94f2d7dfb200aa30e52cb779171f6545f83595cb217a628c")
set(_zstd_comment_4_prior
    "10c315ae609d49b2fc431521aa95908086e69fd917991c5ed9a875af3d1208e8")
set(_zstd_comment_4_prior_count 0)
set(_zstd_comment_4_count 7)
# Alerts: 1650.
string(CONCAT _zstd_comment_4_0_old [=[ZSTD_bounds ZSTD_cParam_getBounds(]=])
string(
  CONCAT _zstd_comment_4_0_new
         [=[/* Purpose: Report the supported range of one comp]=]
         [=[ression parameter.
 * Inputs: param selects a publ]=]
         [=[ic or experimental parameter identifier.
 * Output]=]
         [=[s: Returns inclusive limits and a zero error for s]=]
         [=[upported values;
 *          unknown identifiers r]=]
         [=[eport parameter_unsupported.
 * Limits include bui]=]
         [=[ld-dependent multithreading capability and static
]=]
         [=[ * format limits; this query does not mutate a com]=]
         [=[pression context. */
ZSTD_bounds ZSTD_cParam_getBo]=]
         [=[unds(]=])
# Alerts: 1649.
string(CONCAT _zstd_comment_4_1_old [=[size_t ZSTD_CCtxParams_getParameter(]=])
string(
  CONCAT _zstd_comment_4_1_new
         [=[/* Purpose: Read one configured compression parame]=]
         [=[ter without changing it.
 * Inputs: CCtxParams is ]=]
         [=[an initialized parameter object; value is writable]=]
         [=[;
 *         param identifies the requested public]=]
         [=[ or experimental setting.
 * Outputs: Stores the c]=]
         [=[onfigured integer and returns zero on success;
 * ]=]
         [=[         unsupported identifiers or unavailable wo]=]
         [=[rker features return
 *          parameter_unsuppo]=]
         [=[rted without claiming a valid result. */
size_t ZS]=]
         [=[TD_CCtxParams_getParameter(]=])
# Alerts: 1650.
string(CONCAT _zstd_comment_4_2_old [=[    ZSTD_bounds bounds = { 0, 0, 0 };]=])
string(
  CONCAT _zstd_comment_4_2_new
         [=[    /* Each supported case returns inclusive bound]=]
         [=[s directly.
     * The zero-initialized error dist]=]
         [=[inguishes supported values from the
     * final u]=]
         [=[nsupported-identifier result. */
    ZSTD_bounds b]=]
         [=[ounds = { 0, 0, 0 };]=])
# Alerts: 1650.
string(CONCAT _zstd_comment_4_3_old [=[    case ZSTD_c_nbWorkers:
        bounds.lowerBou]=] [=[nd]=])
string(
  CONCAT _zstd_comment_4_3_new
         [=[    /* A single-thread build exposes only zero wor]=]
         [=[kers and job size.
     * Worker-enabled builds re]=]
         [=[port their compiled upper limits. */
    case ZSTD]=]
         [=[_c_nbWorkers:
        bounds.lowerBound]=])
# Alerts: 1650.
string(CONCAT _zstd_comment_4_4_old [=[    /* experimental parameters */]=])
string(
  CONCAT _zstd_comment_4_4_new
         [=[    /* Experimental identifiers retain their speci]=]
         [=[fic format, enum and
     * resource limits; they ]=]
         [=[are not interchangeable integer switches. */
    /]=]
         [=[* experimental parameters */]=])
# Alerts: 1649.
string(CONCAT _zstd_comment_4_5_old [=[    case ZSTD_c_nbWorkers :
#ifndef ZSTD_MULTITHRE]=] [=[AD
        assert(CCtxParams->nbWorkers == 0);]=])
string(
  CONCAT _zstd_comment_4_5_new
         [=[    /* Worker count remains readable in every buil]=]
         [=[d; the worker-specific
     * settings below rejec]=]
         [=[t requests when multithreading is unavailable. */
]=]
         [=[    case ZSTD_c_nbWorkers :
#ifndef ZSTD_MULTITHRE]=]
         [=[AD
        assert(CCtxParams->nbWorkers == 0);]=])
# Alerts: 1649.
string(CONCAT _zstd_comment_4_6_old [=[    case ZSTD_c_stableInBuffer :
        *value]=])
string(
  CONCAT _zstd_comment_4_6_new
         [=[    /* Stable-buffer modes report the stored contr]=]
         [=[act; querying them does
     * not grant permissio]=]
         [=[n to change buffers during an active frame. */
   ]=]
         [=[ case ZSTD_c_stableInBuffer :
        *value]=])
set(_zstd_comment_5_path "lib/compress/zstd_compress_internal.h")
set(_zstd_comment_5_original
    "ab7754413cef565da559bda7eb738bc2b3708c48877cb9ec37aff767d32c57d1")
set(_zstd_comment_5_patched
    "c3bb0914e9f69bbb997492527308802c1f97d29294310998990277b163f6af46")
set(_zstd_comment_5_prior
    "c3bb0914e9f69bbb997492527308802c1f97d29294310998990277b163f6af46")
set(_zstd_comment_5_prior_count 1)
set(_zstd_comment_5_count 1)
# Alerts: 1600.
string(CONCAT _zstd_comment_5_0_old
              [=[/* ms->nextToUpdate = window->dictLimit; */]=])
string(CONCAT _zstd_comment_5_0_new
              [=[/* Window maintenance updates only window state; m]=]
              [=[atch-state refresh belongs to its caller. */]=])
set(_zstd_comment_6_path "lib/compress/zstd_opt.c")
set(_zstd_comment_6_original
    "625936ee3fb02d789abb894c1fbc2918ee47582434094c963b630da13532c876")
set(_zstd_comment_6_patched
    "4952125ea426e028d0aba6c94175debe49d941e41bc873a52f66d868399b0d32")
set(_zstd_comment_6_prior
    "4952125ea426e028d0aba6c94175debe49d941e41bc873a52f66d868399b0d32")
set(_zstd_comment_6_prior_count 1)
set(_zstd_comment_6_count 1)
# Alerts: 1601.
string(CONCAT _zstd_comment_6_0_old
              [=[/* RAWLOG(2, "%3i:%3i,  ", enb, table[enb]); */]=])
string(CONCAT _zstd_comment_6_0_new
              [=[/* The active diagnostic reports table entries wit]=]
              [=[hout the former paired labels. */]=])
set(_zstd_comment_7_path "lib/decompress/huf_decompress.c")
set(_zstd_comment_7_original
    "710a88d877d5dc5c83a4f839392996f337ec81cf38006c4a5be6042de5b54828")
set(_zstd_comment_7_patched
    "cbac3cc9f9490f35c031587267193f419aefa6a6de352673e2ed008befc34d80")
set(_zstd_comment_7_prior
    "cbac3cc9f9490f35c031587267193f419aefa6a6de352673e2ed008befc34d80")
set(_zstd_comment_7_prior_count 2)
set(_zstd_comment_7_count 2)
# Alerts: 1603.
string(CONCAT _zstd_comment_7_0_old
              [=[/* ZSTD_memset(huffWeight, 0, sizeof(huffWeight));]=]
              [=[ */]=])
string(CONCAT _zstd_comment_7_0_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
# Alerts: 1602.
string(CONCAT _zstd_comment_7_1_old
              [=[/* ZSTD_memset(weightList, 0, sizeof(weightList));]=]
              [=[ */]=])
string(CONCAT _zstd_comment_7_1_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
set(_zstd_comment_8_path "lib/dictBuilder/divsufsort.c")
set(_zstd_comment_8_original
    "2081acb08865f623857d2c0dcb0e79fce9489f01416528c30cfee7097915c616")
set(_zstd_comment_8_patched
    "a2d8d333895d0e54db6af52cc618c60007a0e919a42e58523a3a4dc4d3ddfd9b")
set(_zstd_comment_8_prior
    "6dc014f002149c337ff1871732d1689a3e1d831f470d83adfa574c45d24a290e")
set(_zstd_comment_8_prior_count 1)
set(_zstd_comment_8_count 4)
# Alerts: 1605.
string(CONCAT _zstd_comment_8_0_old
              [=[/*  trbudget_init(&budget, tr_ilg(n) * 3 / 4, n); ]=] [=[*/]=])
string(CONCAT _zstd_comment_8_0_new
              [=[/* Use the selected two-thirds logarithmic budget ]=]
              [=[rather than the earlier three-quarters candidate. ]=] [=[*/]=])
# Alerts: 1651.
string(CONCAT _zstd_comment_8_1_old [=[static
void
ss_mintrosort(]=])
string(
  CONCAT _zstd_comment_8_1_new
         [=[/* Purpose: Sort a medium group of substring suffi]=]
         [=[x indices in place.
 * Inputs: T and PA provide th]=]
         [=[e text and validated suffix starts; first/last
 * ]=]
         [=[        bound the writable group; depth selects th]=]
         [=[e compared character.
 * Outputs: Orders the group]=]
         [=[ and marks equal suffixes with complemented
 *    ]=]
         [=[      indices. Small partitions use insertion sort]=]
         [=[; exhausted depth
 *          budgets use heapsort]=]
         [=[; the bounded stack schedules subgroups. */
static]=]
         "\n"
         [=[void
ss_mintrosort(]=])
# Alerts: 1651.
string(CONCAT _zstd_comment_8_2_old [=[    if(limit-- == 0) { ss_heapsort]=])
string(
  CONCAT _zstd_comment_8_2_new
         [=[    /* The introspection budget bounds repeated pa]=]
         [=[rtitioning; exhausted
     * groups fall back to h]=]
         [=[eapsort before equal-character refinement. */
    ]=]
         [=[if(limit-- == 0) { ss_heapsort]=])
# Alerts: 1651.
string(CONCAT _zstd_comment_8_3_old [=[    if(a <= d) {
      c = b - 1;]=])
string(
  CONCAT _zstd_comment_8_3_new
         [=[    /* Move pivot-equal runs to the middle, then p]=]
         [=[rocess the smaller
     * partition immediately an]=]
         [=[d schedule the others on the fixed stack.
     * O]=]
         [=[nly the equal-character subgroup advances the text]=]
         [=[ depth. */
    if(a <= d) {
      c = b - 1;]=])
set(_zstd_comment_9_path "lib/legacy/zstd_v01.c")
set(_zstd_comment_9_original
    "8f439ec4d83f13caaa4ee86d8de74660a38cade89b48bedf3943a9315b98e167")
set(_zstd_comment_9_patched
    "4e23ed4723901dd04cd2c937e8b67e7034630cbec8efc4d400acca5c8d1e9e2b")
set(_zstd_comment_9_prior
    "7c7d18fed8a4ce47c66c069b01f31fd3be24c15ef2f8741f070b293c10405f38")
set(_zstd_comment_9_prior_count 1)
set(_zstd_comment_9_count 5)
# Alerts: 1606.
string(CONCAT _zstd_comment_9_0_old [=[/* bitTail = bitD1; */]=])
string(CONCAT _zstd_comment_9_0_new
              [=[/* Construct a tail stream directly instead of cop]=]
              [=[ying the preceding stream state. */]=])
# Alerts: 1652.
string(CONCAT _zstd_comment_9_1_old [=[static size_t FSE_readNCount (]=])
string(
  CONCAT _zstd_comment_9_1_new
         [=[/* Purpose: Decode a legacy finite-state entropy n]=]
         [=[ormalized-count header.
 * Inputs: headerBuffer bo]=]
         [=[rrows hbSize bytes; normalizedCounter has space
 *]=]
         [=[         through the input maximum symbol in maxSV]=]
         [=[Ptr; both metadata
 *         pointers are writabl]=]
         [=[e.
 * Outputs: Stores decoded counts, final symbol]=]
         [=[ and table logarithm,
 *          returning consum]=]
         [=[ed bytes or a legacy error code. Zero runs
 *     ]=]
         [=[     and adaptive bit widths follow the versioned ]=]
         [=[wire encoding. */
static size_t FSE_readNCount (]=])
# Alerts: 1652.
string(CONCAT _zstd_comment_9_2_old [=[    bitStream = FSE_readLE32(ip);
    nbBits]=])
string(
  CONCAT _zstd_comment_9_2_new
         [=[    /* Seed the bit reservoir from the first word ]=]
         [=[and decode its table
     * logarithm before deriv]=]
         [=[ing the initial probability mass. */
    bitStream]=]
         [=[ = FSE_readLE32(ip);
    nbBits]=])
# Alerts: 1652.
string(CONCAT _zstd_comment_9_3_old
              [=[    while ((remaining>1) && (charnum<=*maxSVPtr))]=])
string(
  CONCAT _zstd_comment_9_3_new
         [=[    /* Every symbol consumes remaining probability]=]
         [=[ mass; zero-count
     * runs advance the symbol i]=]
         [=[ndex without consuming that mass. */
    while ((r]=]
         [=[emaining>1) && (charnum<=*maxSVPtr))]=])
# Alerts: 1652.
string(CONCAT _zstd_comment_9_4_old [=[    if (remaining != 1)]=])
string(
  CONCAT _zstd_comment_9_4_new
         [=[    /* A complete distribution leaves exactly the ]=]
         [=[encoding sentinel.
     * Publish the final symbol]=]
         [=[ count only after that invariant holds. */
    if ]=]
         [=[(remaining != 1)]=])
set(_zstd_comment_10_path "lib/legacy/zstd_v02.c")
set(_zstd_comment_10_original
    "ffedd2b0e4dae744b8592656050f2c56eb4c8ccad1795fb3efeceb7924808712")
set(_zstd_comment_10_patched
    "dd13bb67a38211c390e1abadb20c8571369c526f0f3ba9d6a39cb17b04c85c66")
set(_zstd_comment_10_prior
    "ffedd2b0e4dae744b8592656050f2c56eb4c8ccad1795fb3efeceb7924808712")
set(_zstd_comment_10_prior_count 0)
set(_zstd_comment_10_count 4)
# Alerts: 1653.
string(CONCAT _zstd_comment_10_0_old [=[static size_t FSE_readNCount (]=])
string(
  CONCAT _zstd_comment_10_0_new
         [=[/* Purpose: Decode a legacy finite-state entropy n]=]
         [=[ormalized-count header.
 * Inputs: headerBuffer bo]=]
         [=[rrows hbSize bytes; normalizedCounter has space
 *]=]
         [=[         through the input maximum symbol in maxSV]=]
         [=[Ptr; both metadata
 *         pointers are writabl]=]
         [=[e.
 * Outputs: Stores decoded counts, final symbol]=]
         [=[ and table logarithm,
 *          returning consum]=]
         [=[ed bytes or a legacy error code. Zero runs
 *     ]=]
         [=[     and adaptive bit widths follow the versioned ]=]
         [=[wire encoding. */
static size_t FSE_readNCount (]=])
# Alerts: 1653.
string(CONCAT _zstd_comment_10_1_old [=[    bitStream = MEM_readLE32(ip);
    nbBits]=])
string(
  CONCAT _zstd_comment_10_1_new
         [=[    /* Seed the bit reservoir from the first word ]=]
         [=[and decode its table
     * logarithm before deriv]=]
         [=[ing the initial probability mass. */
    bitStream]=]
         [=[ = MEM_readLE32(ip);
    nbBits]=])
# Alerts: 1653.
string(CONCAT _zstd_comment_10_2_old
              [=[    while ((remaining>1) && (charnum<=*maxSVPtr))]=])
string(
  CONCAT _zstd_comment_10_2_new
         [=[    /* Every symbol consumes remaining probability]=]
         [=[ mass; zero-count
     * runs advance the symbol i]=]
         [=[ndex without consuming that mass. */
    while ((r]=]
         [=[emaining>1) && (charnum<=*maxSVPtr))]=])
# Alerts: 1653.
string(CONCAT _zstd_comment_10_3_old [=[    if (remaining != 1)]=])
string(
  CONCAT _zstd_comment_10_3_new
         [=[    /* A complete distribution leaves exactly the ]=]
         [=[encoding sentinel.
     * Publish the final symbol]=]
         [=[ count only after that invariant holds. */
    if ]=]
         [=[(remaining != 1)]=])
set(_zstd_comment_11_path "lib/legacy/zstd_v03.c")
set(_zstd_comment_11_original
    "1d69626197b9c76d28012b78fc66bcc3077bf8d796ff6f82457666d3271e7f3d")
set(_zstd_comment_11_patched
    "3966421c4cc3d15fb61b66903bf7a69cf624e38c20f62d79ee0c6e097ab962fb")
set(_zstd_comment_11_prior
    "1d69626197b9c76d28012b78fc66bcc3077bf8d796ff6f82457666d3271e7f3d")
set(_zstd_comment_11_prior_count 0)
set(_zstd_comment_11_count 4)
# Alerts: 1654.
string(CONCAT _zstd_comment_11_0_old [=[static size_t FSE_readNCount (]=])
string(
  CONCAT _zstd_comment_11_0_new
         [=[/* Purpose: Decode a legacy finite-state entropy n]=]
         [=[ormalized-count header.
 * Inputs: headerBuffer bo]=]
         [=[rrows hbSize bytes; normalizedCounter has space
 *]=]
         [=[         through the input maximum symbol in maxSV]=]
         [=[Ptr; both metadata
 *         pointers are writabl]=]
         [=[e.
 * Outputs: Stores decoded counts, final symbol]=]
         [=[ and table logarithm,
 *          returning consum]=]
         [=[ed bytes or a legacy error code. Zero runs
 *     ]=]
         [=[     and adaptive bit widths follow the versioned ]=]
         [=[wire encoding. */
static size_t FSE_readNCount (]=])
# Alerts: 1654.
string(CONCAT _zstd_comment_11_1_old [=[    bitStream = MEM_readLE32(ip);
    nbBits]=])
string(
  CONCAT _zstd_comment_11_1_new
         [=[    /* Seed the bit reservoir from the first word ]=]
         [=[and decode its table
     * logarithm before deriv]=]
         [=[ing the initial probability mass. */
    bitStream]=]
         [=[ = MEM_readLE32(ip);
    nbBits]=])
# Alerts: 1654.
string(CONCAT _zstd_comment_11_2_old
              [=[    while ((remaining>1) && (charnum<=*maxSVPtr))]=])
string(
  CONCAT _zstd_comment_11_2_new
         [=[    /* Every symbol consumes remaining probability]=]
         [=[ mass; zero-count
     * runs advance the symbol i]=]
         [=[ndex without consuming that mass. */
    while ((r]=]
         [=[emaining>1) && (charnum<=*maxSVPtr))]=])
# Alerts: 1654.
string(CONCAT _zstd_comment_11_3_old [=[    if (remaining != 1)]=])
string(
  CONCAT _zstd_comment_11_3_new
         [=[    /* A complete distribution leaves exactly the ]=]
         [=[encoding sentinel.
     * Publish the final symbol]=]
         [=[ count only after that invariant holds. */
    if ]=]
         [=[(remaining != 1)]=])
set(_zstd_comment_12_path "lib/legacy/zstd_v04.c")
set(_zstd_comment_12_original
    "079df3416c7aa806e7e9bb3a2db8c4c65dbe938b0b7e261d7b66591e0e29b551")
set(_zstd_comment_12_patched
    "bdfe34189e97ceb0c8988f306f82226c14fbb56d82709f306acd575c32762071")
set(_zstd_comment_12_prior
    "079df3416c7aa806e7e9bb3a2db8c4c65dbe938b0b7e261d7b66591e0e29b551")
set(_zstd_comment_12_prior_count 0)
set(_zstd_comment_12_count 4)
# Alerts: 1655.
string(CONCAT _zstd_comment_12_0_old [=[static size_t FSE_readNCount (]=])
string(
  CONCAT _zstd_comment_12_0_new
         [=[/* Purpose: Decode a legacy finite-state entropy n]=]
         [=[ormalized-count header.
 * Inputs: headerBuffer bo]=]
         [=[rrows hbSize bytes; normalizedCounter has space
 *]=]
         [=[         through the input maximum symbol in maxSV]=]
         [=[Ptr; both metadata
 *         pointers are writabl]=]
         [=[e.
 * Outputs: Stores decoded counts, final symbol]=]
         [=[ and table logarithm,
 *          returning consum]=]
         [=[ed bytes or a legacy error code. Zero runs
 *     ]=]
         [=[     and adaptive bit widths follow the versioned ]=]
         [=[wire encoding. */
static size_t FSE_readNCount (]=])
# Alerts: 1655.
string(CONCAT _zstd_comment_12_1_old [=[    bitStream = MEM_readLE32(ip);
    nbBits]=])
string(
  CONCAT _zstd_comment_12_1_new
         [=[    /* Seed the bit reservoir from the first word ]=]
         [=[and decode its table
     * logarithm before deriv]=]
         [=[ing the initial probability mass. */
    bitStream]=]
         [=[ = MEM_readLE32(ip);
    nbBits]=])
# Alerts: 1655.
string(CONCAT _zstd_comment_12_2_old
              [=[    while ((remaining>1) && (charnum<=*maxSVPtr))]=])
string(
  CONCAT _zstd_comment_12_2_new
         [=[    /* Every symbol consumes remaining probability]=]
         [=[ mass; zero-count
     * runs advance the symbol i]=]
         [=[ndex without consuming that mass. */
    while ((r]=]
         [=[emaining>1) && (charnum<=*maxSVPtr))]=])
# Alerts: 1655.
string(CONCAT _zstd_comment_12_3_old [=[    if (remaining != 1)]=])
string(
  CONCAT _zstd_comment_12_3_new
         [=[    /* A complete distribution leaves exactly the ]=]
         [=[encoding sentinel.
     * Publish the final symbol]=]
         [=[ count only after that invariant holds. */
    if ]=]
         [=[(remaining != 1)]=])
set(_zstd_comment_13_path "lib/legacy/zstd_v05.c")
set(_zstd_comment_13_original
    "dd60a43788f2a2150ae9ddf91a43ea34c6a03d6a420196e209f5aca2ffb217e2")
set(_zstd_comment_13_patched
    "945d8cb7e5fb2eb9c827753a0b44eadd0ee1307fe290bf26c811de43846756a3")
set(_zstd_comment_13_prior
    "945d8cb7e5fb2eb9c827753a0b44eadd0ee1307fe290bf26c811de43846756a3")
set(_zstd_comment_13_prior_count 8)
set(_zstd_comment_13_count 8)
# Alerts: 1614.
string(CONCAT _zstd_comment_13_0_old [=[/* memset(huffWeight, 0, hwSize); */]=])
string(CONCAT _zstd_comment_13_0_new
              [=[/* The reader fills the Huffman weights before use]=]
              [=[; no preliminary clearing is needed. */]=])
# Alerts: 1613.
string(CONCAT _zstd_comment_13_1_old
              [=[/* memset(huffWeight, 0, sizeof(huffWeight)); */]=])
string(CONCAT _zstd_comment_13_1_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
# Alerts: 1612.
string(CONCAT _zstd_comment_13_2_old
              [=[/* memset(weightList, 0, sizeof(weightList)); */]=])
string(CONCAT _zstd_comment_13_2_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
# Alerts: 1611.
string(CONCAT _zstd_comment_13_3_old
              [=[/* return HUFv05_decompress4X2(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_13_3_new
              [=[/* The single-symbol multistream alternative is su]=]
              [=[perseded by the selected decoder above. */]=])
# Alerts: 1610.
string(CONCAT _zstd_comment_13_4_old
              [=[/* return HUFv05_decompress4X4(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_13_4_new
              [=[/* The double-symbol multistream alternative is su]=]
              [=[perseded by the selected decoder above. */]=])
# Alerts: 1609.
string(CONCAT _zstd_comment_13_5_old
              [=[/* return HUFv05_decompress4X6(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_13_5_new
              [=[/* The quad-symbol multistream alternative is supe]=]
              [=[rseded by the selected decoder above. */]=])
# Alerts: 1608.
string(CONCAT _zstd_comment_13_6_old
              [=[/* zbc->stage = ZBUFFv05ds_decodeHeader; break; */]=])
string(CONCAT _zstd_comment_13_6_new
              [=[/* Fall through to header decoding without a redun]=]
              [=[dant stage assignment. */]=])
# Alerts: 1607.
string(CONCAT _zstd_comment_13_7_old [=[/* break; */]=])
string(CONCAT _zstd_comment_13_7_new
              [=[/* Fall through to the flush stage. */]=])
set(_zstd_comment_14_path "lib/legacy/zstd_v06.c")
set(_zstd_comment_14_original
    "d2cadc9e2906fe50e9c9564bdcb291582532060fdcec9c4bc5f0ee4370182b75")
set(_zstd_comment_14_patched
    "6df64f285f652ab03ae182d8eea46970956830b4205fc9f0471b6edd66287b42")
set(_zstd_comment_14_prior
    "6df64f285f652ab03ae182d8eea46970956830b4205fc9f0471b6edd66287b42")
set(_zstd_comment_14_prior_count 9)
set(_zstd_comment_14_count 9)
# Alerts: 1623.
string(CONCAT _zstd_comment_14_0_old
              [=[/* 0 == no longLength; 1 == Lit.longLength; 2 == M]=]
              [=[atch.longLength; */]=])
string(CONCAT _zstd_comment_14_0_new
              [=[/* Long-length kind: zero means absent, one means ]=]
              [=[literals, two means matches. */]=])
# Alerts: 1622.
string(CONCAT _zstd_comment_14_1_old [=[/* memset(huffWeight, 0, hwSize); */]=])
string(CONCAT _zstd_comment_14_1_new
              [=[/* The reader fills the Huffman weights before use]=]
              [=[; no preliminary clearing is needed. */]=])
# Alerts: 1621.
string(CONCAT _zstd_comment_14_2_old
              [=[/* memset(huffWeight, 0, sizeof(huffWeight)); */]=])
string(CONCAT _zstd_comment_14_2_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
# Alerts: 1620.
string(CONCAT _zstd_comment_14_3_old
              [=[/* memset(weightList, 0, sizeof(weightList)); */]=])
string(CONCAT _zstd_comment_14_3_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
# Alerts: 1619.
string(CONCAT _zstd_comment_14_4_old
              [=[/* if (Dtime[2] < Dtime[algoNb]) algoNb = 2; */]=])
string(CONCAT _zstd_comment_14_4_new
              [=[/* The quad-symbol decoder is excluded from this s]=]
              [=[peed-based selection. */]=])
# Alerts: 1618.
string(CONCAT _zstd_comment_14_5_old
              [=[/* return HUFv06_decompress4X2(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_14_5_new
              [=[/* The single-symbol multistream alternative is su]=]
              [=[perseded by the selected decoder above. */]=])
# Alerts: 1617.
string(CONCAT _zstd_comment_14_6_old
              [=[/* return HUFv06_decompress4X4(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_14_6_new
              [=[/* The double-symbol multistream alternative is su]=]
              [=[perseded by the selected decoder above. */]=])
# Alerts: 1616.
string(CONCAT _zstd_comment_14_7_old
              [=[/* return HUFv06_decompress4X6(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_14_7_new
              [=[/* The quad-symbol multistream alternative is supe]=]
              [=[rseded by the selected decoder above. */]=])
# Alerts: 1615.
string(CONCAT _zstd_comment_14_8_old [=[/* break; */]=])
string(CONCAT _zstd_comment_14_8_new
              [=[/* Fall through to the flush stage. */]=])
set(_zstd_comment_15_path "lib/legacy/zstd_v07.c")
set(_zstd_comment_15_original
    "ae9f3c0a440b0f61d0ce44b06ee7ed3d256e10f3cb7dbbd9834d6c13625ac944")
set(_zstd_comment_15_patched
    "c024d6594f0644d134c0834c3d05c9ceec29078ae1217a18a41589b1f9b1d5dc")
set(_zstd_comment_15_prior
    "c024d6594f0644d134c0834c3d05c9ceec29078ae1217a18a41589b1f9b1d5dc")
set(_zstd_comment_15_prior_count 10)
set(_zstd_comment_15_count 10)
# Alerts: 1633.
string(CONCAT _zstd_comment_15_0_old [=[/* memset(huffWeight, 0, hwSize); */]=])
string(CONCAT _zstd_comment_15_0_new
              [=[/* The reader fills the Huffman weights before use]=]
              [=[; no preliminary clearing is needed. */]=])
# Alerts: 1632.
string(CONCAT _zstd_comment_15_1_old
              [=[/* memset(huffWeight, 0, sizeof(huffWeight)); */]=])
string(CONCAT _zstd_comment_15_1_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
# Alerts: 1631.
string(CONCAT _zstd_comment_15_2_old
              [=[/* memset(weightList, 0, sizeof(weightList)); */]=])
string(CONCAT _zstd_comment_15_2_new
              [=[/* The weight reader initializes the used portion ]=]
              [=[of this temporary array. */]=])
# Alerts: 1630.
string(CONCAT _zstd_comment_15_3_old
              [=[/* return HUFv07_decompress4X2(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_15_3_new
              [=[/* The single-symbol multistream alternative is su]=]
              [=[perseded by the selected decoder above. */]=])
# Alerts: 1629.
string(CONCAT _zstd_comment_15_4_old
              [=[/* return HUFv07_decompress4X4(dst, dstSize, cSrc,]=]
              [=[ cSrcSize); */]=])
string(CONCAT _zstd_comment_15_4_new
              [=[/* The double-symbol multistream alternative is su]=]
              [=[perseded by the selected decoder above. */]=])
# Alerts: 1628.
string(CONCAT _zstd_comment_15_5_old
              [=[/* printf("alloc %p, %d opaque=%p \n", address, (i]=]
              [=[nt)size, opaque); */]=])
string(CONCAT _zstd_comment_15_5_new
              [=[/* The default allocator forwards the requested si]=]
              [=[ze without diagnostic output. */]=])
# Alerts: 1627.
string(CONCAT _zstd_comment_15_6_old
              [=[/* if (address) printf("free %p opaque=%p \n", add]=]
              [=[ress, opaque); */]=])
string(CONCAT _zstd_comment_15_6_new
              [=[/* The default release callback accepts null stora]=]
              [=[ge without diagnostic output. */]=])
# Alerts: 1626.
string(CONCAT _zstd_comment_15_7_old
              [=[/* 0 == no longLength; 1 == Lit.longLength; 2 == M]=]
              [=[atch.longLength; */]=])
string(CONCAT _zstd_comment_15_7_new
              [=[/* Long-length kind: zero means absent, one means ]=]
              [=[literals, two means matches. */]=])
# Alerts: 1625.
string(CONCAT _zstd_comment_15_8_old
              [=[/* if (litPtr > litEnd) return ERROR(corruption_de]=]
              [=[tected); */]=])
string(CONCAT _zstd_comment_15_8_new
              [=[/* Successful sequence execution has already bound]=]
              [=[ed the consumed literal extent. */]=])
# Alerts: 1624.
string(CONCAT _zstd_comment_15_9_old [=[/* break; */]=])
string(CONCAT _zstd_comment_15_9_new
              [=[/* Fall through to the flush stage. */]=])
set(_zstd_comment_16_path "lib/zdict.h")
set(_zstd_comment_16_original
    "abacadb94e3f79e591f4b1648e839b0160fbf4291211fd01bdba1380269b245c")
set(_zstd_comment_16_patched
    "65f365e97b6c7b4ba47f1dfa0d76c9ab5fce62ce8c5394390d76c2136c1d1384")
set(_zstd_comment_16_prior
    "65f365e97b6c7b4ba47f1dfa0d76c9ab5fce62ce8c5394390d76c2136c1d1384")
set(_zstd_comment_16_prior_count 1)
set(_zstd_comment_16_count 1)
# Alerts: 1604.
string(CONCAT _zstd_comment_16_0_old
              [=[/**< Write log to stderr; 0 = none (default); 1 = ]=]
              [=[errors; 2 = progression; 3 = details; 4 = debug; *]=] [=[/]=])
string(
  CONCAT _zstd_comment_16_0_new
         [=[/**< Write logs to stderr: zero disables logs, one]=]
         [=[ reports errors, two progress, three details, four]=]
         [=[ debug. */]=])
