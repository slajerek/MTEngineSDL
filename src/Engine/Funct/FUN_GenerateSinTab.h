#ifndef _FUN_GenerateSinTab_h_
#define _FUN_GenerateSinTab_h_

// FIXME: note template does not link error?
template <typename T> void FUN_GenerateScaledSinTab(int num_elements, double min_value, double max_value, T *sine_table);
template <typename T> void FUN_PrintTableToStdout(int num_elements, T *sine_table);

void FUN_GenerateScaledSinTabInt(int num_elements, double min_value, double max_value, int *sine_table);

#endif
