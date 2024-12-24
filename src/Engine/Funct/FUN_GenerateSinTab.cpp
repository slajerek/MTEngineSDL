#include "DBG_Log.h"
#include "FUN_GenerateSinTab.h"
#include "SYS_Defs.h"

template <typename T> void FUN_GenerateScaledSinTab(int num_elements, double min_value, double max_value, T *sine_table)
{
	double amplitude = (max_value - min_value) / 2.0;
	double offset = (max_value + min_value) / 2.0;

	// Generating and scaling sine wave values
	for (int i = 0; i < num_elements; i++) {
		double angle = (2.0 * MATH_PI * i) / (double)(num_elements);
		double sine_value = sin(angle);
		sine_table[i] = (T)((sine_value * amplitude) + offset);
	}
}

template <typename T> void FUN_PrintTableToStdout(int num_elements, T *sine_table)
{
	std::cout << "T sine_wave[" << num_elements << "] = {\n";
	for (int i = 0; i < num_elements; i++) {
		if (i % 8 == 0) {
			std::cout << "    ";
		}
		std::cout << static_cast<int>(sine_table[i]);
		if (i < num_elements - 1) {
			std::cout << ", ";
		}
		if ((i + 1) % 8 == 0) {
			std::cout << "\n";
		}
	}
	std::cout << "\n};\n";
}

void FUN_GenerateScaledSinTabInt(int num_elements, double min_value, double max_value, int *sine_table)
{
	double amplitude = (max_value - min_value) / 2.0;
	double offset = (max_value + min_value) / 2.0;

	// Generating and scaling sine wave values
	for (int i = 0; i < num_elements; i++) {
		double angle = (2.0 * MATH_PI * i) / (double)(num_elements);
		double sine_value = sin(angle);
		sine_table[i] = (int)((sine_value * amplitude) + offset);
	}
}
