// sandbox.h - after start-up, lock the server down to only what a server must do.
#pragma once
void fk_sandbox(void);                 // Landlock (files + network) and seccomp (system calls)
const char *fk_sandbox_status(void);   // for the start message and /debug
