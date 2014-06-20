#include "HydroGPU/Solver.h"
#include "HydroGPU/HydroGPUApp.h"
#include "Common/File.h"

Solver::Solver(
	HydroGPUApp& app_,
	const std::string& programFilename)
: app(app_)
, commands(app.commands)
{
	cl::Device device = app.device;
	cl::Context context = app.context;
	
	stateBoundaryKernels.resize(NUM_BOUNDARY_METHODS);
	for (std::vector<cl::Kernel>& v : stateBoundaryKernels) {
		v.resize(app.dim);
	}
	
	// NDRanges

#if 0
	Tensor::Vector<size_t,3> localSizeVec;
	size_t maxWorkGroupSize = device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
	std::vector<size_t> maxWorkItemSizes = device.getInfo<CL_DEVICE_MAX_WORK_ITEM_SIZES>();
	for (int n = 0; n < 3; ++n) {
		localSizeVec(n) = std::min<size_t>(maxWorkItemSizes[n], app.size.s[n]);
	}
	while (localSizeVec.volume() > maxWorkGroupSize) {
		for (int n = 0; n < 3; ++n) {
			localSizeVec(n) = (size_t)ceil((double)localSizeVec(n) * .5);
		}
	}
#endif	
	
	//if dim 2 is size 1 then tell opencl to treat it like a 1D problem
	switch (app.dim) {
	case 1:
		globalSize = cl::NDRange(app.size.s[0]);
		localSize = cl::NDRange(16);
		localSize1d = cl::NDRange(localSize[0]);
		offset1d = cl::NDRange(0);
		offsetNd = cl::NDRange(0);
		break;
	case 2:
		globalSize = cl::NDRange(app.size.s[0], app.size.s[1]);
		localSize = cl::NDRange(16, 16);
		localSize1d = cl::NDRange(localSize[0]);
		offset1d = cl::NDRange(0);
		offsetNd = cl::NDRange(0, 0);
		break;
	case 3:
		globalSize = cl::NDRange(app.size.s[0], app.size.s[1], app.size.s[2]);
		localSize = cl::NDRange(8, 8, 8);
		localSize1d = cl::NDRange(localSize[0]);
		offset1d = cl::NDRange(0);
		offsetNd = cl::NDRange(0, 0, 0);
		break;
	}
	
	std::cout << "global_size\t" << globalSize << std::endl;
	std::cout << "local_size\t" << localSize << std::endl;
	
	{
		std::vector<std::string> commonFilenames = {
			"Common2D.cl",
			"Common2D.cl",
			"Common3D.cl"
		};
		
		std::vector<std::string> kernelSources = std::vector<std::string>{
			std::string() + "#define GAMMA " + std::to_string(app.gamma) + "f\n",
			std::string() + "#define DIM " + std::to_string(app.dim) + "\n",
			Common::File::read(commonFilenames[app.dim-1]),
			Common::File::read(programFilename)
		};
		std::vector<std::pair<const char *, size_t>> sources;
		for (const std::string &s : kernelSources) {
			sources.push_back(std::pair<const char *, size_t>(s.c_str(), s.length()));
		}
		program = cl::Program(context, sources);
	}

	try {
		program.build({device}, "-I include");
	} catch (cl::Error &err) {
		throw Common::Exception() 
			<< "failed to build program executable!\n"
			<< program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device);
	}

	//warnings?
	std::cout << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device) << std::endl;
	
	int volume = app.size.s[0] * app.size.s[1] * app.size.s[2];
	
	cflBuffer = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(real) * volume);
	cflSwapBuffer = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(real) * volume / localSize[0]);
	dtBuffer = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(real));
	gravityPotentialBuffer = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(real) * volume);
	
	//get the edges, so reduction doesn't
	{
		std::vector<real> cflVec(volume);
		for (real &r : cflVec) { r = std::numeric_limits<real>::max(); }
		commands.enqueueWriteBuffer(cflBuffer, CL_TRUE, 0, sizeof(real) * volume, &cflVec[0]);
	}
	
	if (app.useFixedDT) {
		commands.enqueueWriteBuffer(dtBuffer, CL_TRUE, 0, sizeof(real), &app.fixedDT);
	}
}

void Solver::initKernels() {
	
	int volume = app.size.s[0] * app.size.s[1] * app.size.s[2];
	
	for (int boundaryIndex = 0; boundaryIndex < NUM_BOUNDARY_METHODS; ++boundaryIndex) {
		for (int side = 0; side < app.dim; ++side) {
			std::string name = "stateBoundary";
			switch (boundaryIndex) {
			case BOUNDARY_PERIODIC:
				name += "Periodic";
				break;
			case BOUNDARY_MIRROR:
				name += "Mirror";
				break;
			case BOUNDARY_FREEFLOW:
				name += "FreeFlow";
				break;
			default:
				throw Common::Exception() << "no kernel for boundary method " << boundaryIndex;
			}
			switch (side) {
			case 0:
				name += "X";
				break;
			case 1:
				name += "Y";
				break;
			case 2:
				name += "Z";
				break;
			}
			stateBoundaryKernels[boundaryIndex][side] = cl::Kernel(program, name.c_str());
			app.setArgs(stateBoundaryKernels[boundaryIndex][side], stateBuffer, app.size);
		}
	}
	
	calcCFLMinReduceKernel = cl::Kernel(program, "calcCFLMinReduce");
	app.setArgs(calcCFLMinReduceKernel, cflBuffer, cl::Local(localSize[0] * sizeof(real)), volume, cflSwapBuffer);
	
	poissonRelaxKernel = cl::Kernel(program, "poissonRelax");
	app.setArgs(poissonRelaxKernel, gravityPotentialBuffer, stateBuffer, app.size, app.dx);
}

void Solver::findMinTimestep() {
	int reduceSize = app.size.s[0] * app.size.s[1] * app.size.s[2];
	cl::Buffer dst = cflSwapBuffer;
	cl::Buffer src = cflBuffer;
	while (reduceSize > 1) {
		int nextSize = (reduceSize >> 4) + !!(reduceSize & ((1 << 4) - 1));
		cl::NDRange reduceGlobalSize(std::max<int>(reduceSize, localSize[0]));
		calcCFLMinReduceKernel.setArg(0, src);
		calcCFLMinReduceKernel.setArg(2, reduceSize);
		calcCFLMinReduceKernel.setArg(3, nextSize == 1 ? dtBuffer : dst);
		commands.enqueueNDRangeKernel(calcCFLMinReduceKernel, offset1d, reduceGlobalSize, localSize1d);
		commands.finish();
		std::swap(dst, src);
		reduceSize = nextSize;
	}
}

void Solver::update() {
	if (app.showTimestep) {
		real dt;
		commands.enqueueReadBuffer(dtBuffer, CL_TRUE, 0, sizeof(real), &dt);
		std::cout << "dt " << dt << std::endl;
	}
}

void Solver::setPoissonRelaxRepeatArg() {
	cl_int3 repeat;
	for (int i = 0; i < app.dim; ++i) {
		switch (app.boundaryMethods(0)) {	//TODO per dimension
		case BOUNDARY_PERIODIC:
			repeat.s[i] = 1;
			break;
		case BOUNDARY_MIRROR:
		case BOUNDARY_FREEFLOW:
			repeat.s[i] = 0;
			break;
		default:
			throw Common::Exception() << "unknown boundary method " << app.boundaryMethods(0);
		}	
	}	
	poissonRelaxKernel.setArg(4, repeat);
}

