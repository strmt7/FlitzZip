/* Purpose: Verify C compilation stays independent of the C++ PCH.
 * Inputs: None. Outputs: A fixed cross-language value. */
unsigned int fixture_c_value(void) {
    return 3U;
}
