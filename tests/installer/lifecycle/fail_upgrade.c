// Purpose: Fail after deferred InstallFiles to prove rollback of the previous product and payload.
// Inputs: Borrowed Windows Installer session, unused. Outputs: ERROR_INSTALL_FAILURE; no host side effects.
__declspec(dllexport) unsigned int __stdcall RejectFixtureUpgrade(unsigned int session) {
    (void)session;
    return 1603U;
}
