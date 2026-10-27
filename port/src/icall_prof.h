#ifndef EW2_ICALL_PROF_H
#define EW2_ICALL_PROF_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional profiler for a few Mono internal calls that scan or build the scene (Find*, Resources.Load,
 * Instantiate). EW2_ICALL_PROF=1. icall_prof_wrap() is asked for every icall the engine registers and returns
 * either the function itself or a timing wrapper; icall_prof_tick() prints a line per wrapped call every
 * 120 frames ("icallprof: ..."), with the searched C# type for the Find* calls.
 */
void *icall_prof_wrap(const char *name, void *fn);
void icall_prof_tick(long frame);

#ifdef __cplusplus
}
#endif

#endif
