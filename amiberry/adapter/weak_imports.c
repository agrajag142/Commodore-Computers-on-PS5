/* weak_imports.c - PS5 port (built with the app, not in an archive, which a
 * weak reference would not pull in): weak symbols the code checks for before using.
 * In a normal executable a missing one is left at 0; the kit's converter
 * wants every import found, so they are defined here instead. */

/* thread_local fixup_prefs_preview_active (main.cpp) has no dynamic
 * initialisation: its "initialise me" function does nothing */
void _ZTH26fixup_prefs_preview_active(void) {}

/* libc++abi registers thread_local destructors through this when it is
 * there: here they are not run when a thread ends (the app's threads live as
 * long as the app) */
int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso) {
  (void)dtor;
  (void)obj;
  (void)dso;
  return 0;
}
