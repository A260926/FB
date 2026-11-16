// Simple wrapper for Sven Forstmann's mesh simplification tool
//
// Loads a OBJ format mesh, decimates mesh, saves decimated mesh as OBJ format
// http://voxels.blogspot.com/2014/05/quadric-mesh-simplification-with-source.html
// https://github.com/sp4cerat/Fast-Quadric-Mesh-Simplification
//To compile for Linux/OSX (GCC/LLVM)
//  g++ Main.cpp -O3 -o simplify
//To compile for Windows (Visual Studio)
// vcvarsall amd64
// cl /EHsc Main.cpp /osimplify
//To execute
//  ./simplify wall.obj out.obj 0.04
//
// Pascal Version by Chris Roden:
// https://github.com/neurolabusc/Fast-Quadric-Mesh-Simplification-Pascal-
// WebAssembly Port by MyMiniFactory
//  https://github.com/MyMiniFactory/Fast-Quadric-Mesh-Simplification
// To compile for WebAssembly
//  make wasm

#include "Simplify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>  // clock_t, clock, CLOCKS_PER_SEC
#include <vector>
#include <string>
#include <cmath>
#include <cerrno>

void showHelp(const char * argv[]) {
	const char *cstr = (argv[0]);
	printf("Usage: %s <input> <output> <ratio> <agressiveness> [--symmetry <file>]\n", cstr);
	printf(" Input: name of existing OBJ format mesh\n");
	printf(" Output: name for decimated OBJ format mesh\n");
	printf(" Ratio: (default = 0.5) for example 0.2 will decimate 80%% of triangles\n");
	printf(" Agressiveness: (default = 7.0) faster or better decimation\n");
	printf(" --symmetry file: reflection symmetry constraint; the planar mirror is\n");
	printf("   given by three normal components, offset, input tolerance, vertex\n");
	printf("   count and one mate index per vertex (whitespace separated).\n");
	printf("   The compacted mate is written to <output>.symmetry\n");
	printf("Examples :\n");
	#if defined(_WIN64) || defined(_WIN32)
		printf("  %s c:\\dir\\in.obj c:\\dir\\out.obj 0.2\n", cstr);
	#else
		printf("  %s ~/dir/in.obj ~/dir/out.obj 0.2\n", cstr);
	#endif
} //showHelp()

bool is_extension(const char* file_path, const char* extension) {
	char file_extension[3];
	file_extension[0] = file_path[strlen(file_path)-3];
	file_extension[1] = file_path[strlen(file_path)-2];
	file_extension[2] = file_path[strlen(file_path)-1];
	return (file_extension[0] == extension[0]
		and file_extension[1] == extension[1]
		and file_extension[2] == extension[2]);
}

bool is_obj(const char* file_path) {
	return is_extension(file_path, "obj");
}

bool is_mz3(const char* file_path) {
	return is_extension(file_path, "mz3");
}

// Read a symmetry description: nx ny offset tolerance vertex_count mate...
bool load_symmetry(const char* file_path, Simplify::SymmetryContext &ctx) {
	FILE *f = fopen(file_path, "rb");
	if (!f) { printf("Unable to open symmetry file %s\n", file_path); return false; }
	std::vector<double> vals;
	char token[256];
	while (fscanf(f, "%255s", token) == 1) {
		char *end = 0;
		errno = 0;
		double v = strtod(token, &end);
		if (end == token || *end != 0 || !std::isfinite(v)) {
			printf("Invalid number '%s' in symmetry file\n", token);
			fclose(f);
			return false;
		}
		vals.push_back(v);
	}
	bool ok = ferror(f) == 0;
	fclose(f);
	if (!ok || vals.size() < 7) { printf("Symmetry file too short\n"); return false; }
	ctx.normal = vec3f(vals[0], vals[1], vals[2]);
	ctx.offset = vals[3];
	ctx.input_tolerance = vals[4];
	double countd = vals[5];
	if (!(countd >= 0) || countd != std::floor(countd)) { printf("Bad vertex count\n"); return false; }
	size_t count = (size_t)countd;
	if (vals.size() != 6 + count) { printf("Wrong mate count\n"); return false; }
	ctx.mate.resize(count);
	for (size_t i = 0; i < count; ++i) {
		double m = vals[6+i];
		if (m != std::floor(m) || m < 0 || m >= (double)count) { printf("Bad mate index\n"); return false; }
		ctx.mate[i] = (int)m;
	}
	return true;
}

int simplify(const char* file_path, const char* export_path, float reduceFraction, float agressiveness,
	const char* symmetry_path) {

	printf("Mesh Simplification (C)2014 by Sven Forstmann in 2014, MIT License (%zu-bit)\n", sizeof(size_t)*8);

	if (is_obj(file_path)) {
		Simplify::load_obj(file_path, true);
		printf("loading obj\n");
	}
	else if (is_mz3(file_path)) {
		printf("loading mz3\n");
		Simplify::load_mz3(file_path);
	} else {
		printf("file is not obj or mz3 %s\n", file_path);
		return EXIT_FAILURE;
	}

	if ((Simplify::triangles.size() < 3) || (Simplify::vertices.size() < 3)) {
		printf("triangles size or vertices size less than 3\n");
		return EXIT_FAILURE;
	}

	int target_count =  Simplify::triangles.size() >> 1;

	if (reduceFraction > 1.0) reduceFraction = 1.0; //lossless only
	if (reduceFraction <= 0.0) {
		printf("Ratio must be BETWEEN zero and one.\n");
		return EXIT_FAILURE;
	}
	target_count = round((float)Simplify::triangles.size() * reduceFraction);

	if (target_count < 4) {
		printf("Object will not survive such extreme decimation\n");
		return EXIT_FAILURE;
	}
	clock_t start = clock();
	Simplify::SymmetryContext sym;
	bool use_sym = false;
	if (symmetry_path) {
		if (!load_symmetry(symmetry_path, sym)) return EXIT_FAILURE;
		if (sym.mate.size() != Simplify::vertices.size()) {
			printf("Symmetry vertex count does not match mesh\n");
			return EXIT_FAILURE;
		}
		use_sym = true;
	}
	printf("Input: %zu vertices, %zu triangles (target %d)\n", Simplify::vertices.size(), Simplify::triangles.size(), target_count);
	int startSize = Simplify::triangles.size();
	try {
		Simplify::simplify_mesh(target_count, agressiveness, true, use_sym ? &sym : 0);
	} catch (const std::invalid_argument &e) {
		printf("symmetry error: %s\n", e.what());
		return EXIT_FAILURE;
	}
	//Simplify::simplify_mesh_lossless( false);
	if ( Simplify::triangles.size() >= startSize) {
		printf("Unable to reduce mesh.\n");
		return EXIT_FAILURE;
	}

	if (is_obj(export_path)) {
		printf("exporting obj\n");
		Simplify::write_obj(export_path);
	}
	else if (is_mz3(export_path)) {
		printf("exporting mz3\n");
		Simplify::write_mz3(export_path);
	} else {
		printf("export file is not obj or stl %s\n", export_path);
		return EXIT_FAILURE;
	}

	if (use_sym) {
		std::string sym_path(export_path);
		sym_path += ".symmetry";
		FILE *sf = fopen(sym_path.c_str(), "w");
		if (!sf) { printf("Unable to write %s\n", sym_path.c_str()); return EXIT_FAILURE; }
		fprintf(sf, "%.17g %.17g %.17g\n", sym.normal.x, sym.normal.y, sym.normal.z);
		fprintf(sf, "%.17g\n%.17g\n", sym.offset, sym.input_tolerance);
		fprintf(sf, "%zu\n", sym.mate.size());
		for (size_t i = 0; i < sym.mate.size(); ++i)
			fprintf(sf, "%d%c", sym.mate[i], (i+1)%16 == 0 ? '\n' : ' ');
		fprintf(sf, "\n");
		fclose(sf);
	}
	printf("Output: %zu vertices, %zu triangles (%f reduction; %.4f sec)\n",Simplify::vertices.size(), Simplify::triangles.size()
		, (float)Simplify::triangles.size()/ (float) startSize  , ((float)(clock()-start))/CLOCKS_PER_SEC );
	return EXIT_SUCCESS;
}

#ifdef __EMSCRIPTEN__

extern "C" {
int simplify(const char* file_path, float reduceFraction, const char* export_path, float agressiveness) {
	printf("Going to simplify %s\n", file_path);
	return simplify(file_path, export_path, reduceFraction, agressiveness, 0);
}
}

#else

int main(int argc, const char * argv[]) {
	if (argc < 3) {
		showHelp(argv);
		return EXIT_SUCCESS;
	}

	const char* file_path = argv[1];
	const char* export_path = argv[2];
	const char* positional[2] = {0, 0};
	int npos = 0;
	const char* symmetry_path = 0;
	for (int i = 3; i < argc; ++i) {
		if (strcmp(argv[i], "--symmetry") == 0) {
			if (i + 1 >= argc) {
				printf("--symmetry requires a file argument\n");
				return EXIT_FAILURE;
			}
			symmetry_path = argv[++i];
		} else if (npos < 2) {
			positional[npos++] = argv[i];
		} else {
			showHelp(argv);
			return EXIT_FAILURE;
		}
	}
	float reduceFraction = 0.5;
	if (npos > 0) reduceFraction = atof(positional[0]);
	float agressiveness = 7.0;
	if (npos > 1) agressiveness = atof(positional[1]);
	return simplify(file_path, export_path, reduceFraction, agressiveness, symmetry_path);
}

#endif