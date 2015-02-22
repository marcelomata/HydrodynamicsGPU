#include "HydroGPU/Solver/Solver.h"
#include "HydroGPU/Integrator/ForwardEuler.h"
#include "HydroGPU/Integrator/RungeKutta4.h"
#include "HydroGPU/Plot/VectorField.h"
#include "HydroGPU/Plot/Plot1D.h"
#include "HydroGPU/Plot/Plot2D.h"
#include "HydroGPU/Plot/Plot3D.h"
#include "HydroGPU/HydroGPUApp.h"
#include "Image/System.h"
#include "Common/File.h"

namespace HydroGPU {
namespace Solver {

cl::Buffer Solver::clAlloc(size_t size) {
	totalAlloc += size;
	std::cout << "allocating gpu mem size " << size << " running total " << totalAlloc << std::endl; 
	return cl::Buffer(app->context, CL_MEM_READ_WRITE, size);
}

Solver::Solver(HydroGPUApp* app_)
: app(app_)
, commands(app->commands)
, totalAlloc(0)
{
}

void Solver::init() {
	//we need this first, so don't trust child classes to assign it prior to calling Super::init
	//instead make them provide this method
	//TODO non-virtual init() and make it call out construction code in a particular order
	createEquation();

	cl::Device device = app->device;
	
	// NDRanges

#if 0
	Tensor::Vector<size_t,3> localSizeVec;
	size_t maxWorkGroupSize = device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
	std::vector<size_t> maxWorkItemSizes = device.getInfo<CL_DEVICE_MAX_WORK_ITEM_SIZES>();
	for (int n = 0; n < 3; ++n) {
		localSizeVec(n) = std::min<size_t>(maxWorkItemSizes[n], app->size.s[n]);
	}
	while (localSizeVec.volume() > maxWorkGroupSize) {
		for (int n = 0; n < 3; ++n) {
			localSizeVec(n) = (size_t)ceil((double)localSizeVec(n) * .5);
		}
	}
#endif	

/*
What's this for?
I was going to useGPU=false for debugging, so I could use a kernel size of 1 and output debug stuff sequentially
... but I'm getting crashes inside of kernels *only* when GPU is disabled.
SO I thought maybe I could useGPU=true and still output sequentially...
but there's currently an issue with the reduce kernel when doing that, which I need to disable.
OR I could just have the debug printfs also output their thread ID and filter all the debug output
*/
//#define DEBUG_OVERRIDE
	
	//if dim 2 is size 1 then tell opencl to treat it like a 1D problem
	switch (app->dim) {
	case 1:
		globalSize = cl::NDRange(app->size.s[0]);
		{	
			int n = app->useGPU ? 16 : 1;
#ifdef DEBUG_OVERRIDE
			n = 1;
#endif
			localSize = cl::NDRange(n);
		}
		localSize1d = cl::NDRange(localSize[0]);
		offset1d = cl::NDRange(0);
		offsetNd = cl::NDRange(0);
		break;
	case 2:
		globalSize = cl::NDRange(app->size.s[0], app->size.s[1]);
		{
			int n = app->useGPU ? 16 : 1;
#ifdef DEBUG_OVERRIDE
			n = 1;
#endif
			localSize = cl::NDRange(n, n);
		}
		localSize1d = cl::NDRange(localSize[0]);
		offset1d = cl::NDRange(0);
		offsetNd = cl::NDRange(0, 0);
		break;
	case 3:
		globalSize = cl::NDRange(app->size.s[0], app->size.s[1], app->size.s[2]);
		{
			int n = app->useGPU ? 8 : 1;
#ifdef DEBUG_OVERRIDE
			n = 1;
#endif
			localSize = cl::NDRange(n, n, n);
		}
		localSize1d = cl::NDRange(localSize[0]);
		offset1d = cl::NDRange(0);
		offsetNd = cl::NDRange(0, 0, 0);
		break;
	}


	std::cout << "global_size\t" << globalSize << std::endl;
	std::cout << "local_size\t" << localSize << std::endl;
	
	{
		std::vector<std::string> sourceStrs = getProgramSources();
		std::vector<std::pair<const char *, size_t>> sources;
		for (const std::string &s : sourceStrs) {
			sources.push_back(std::pair<const char *, size_t>(s.c_str(), s.length()));
		}
		program = cl::Program(app->context, sources);
	}

	try {
		program.build({device}, "-I include");// -Werror -cl-fast-relaxed-math");
	} catch (cl::Error &err) {
		throw Common::Exception() 
			<< "failed to build program executable!\n"
			<< program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device);
	}

	//warnings?
	std::cout << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(device) << std::endl;

	//for curiousity's sake
	if (app->useGPU) {
		cl_int err;
		
		size_t size = 0;
		err = clGetProgramInfo(program(), CL_PROGRAM_BINARY_SIZES, sizeof(size_t), &size, nullptr);
		if (err != CL_SUCCESS) throw Common::Exception() << "failed to get binary size";
	
		std::vector<char> binary(size);
		err = clGetProgramInfo(program(), CL_PROGRAM_BINARIES, size, &binary[0], nullptr);
		if (err != CL_SUCCESS) throw Common::Exception() << "failed to get binary";

		Common::File::write("program.cl.bin", std::string(&binary[0], binary.size()));
	}

	initBuffers();
	initKernels();

	//create integrator
	std::string integratorName = "ForwardEuler";
	app->lua.ref()["integratorName"] >> integratorName;
	if (integratorName == "ForwardEuler") {
		integrator = std::make_shared<HydroGPU::Integrator::ForwardEuler>(this);
	} else if (integratorName == "RungeKutta4") {
		integrator = std::make_shared<HydroGPU::Integrator::RungeKutta4>(this);
	} else {
		throw Common::Exception() << "failed to find integrator named " << integratorName;
	}
}


void Solver::initBuffers() {
	int volume = getVolume();

	//not necessary for fixed timestep.  TODO don't allocate in that case.
	cflBuffer = clAlloc(sizeof(real) * volume);
	cflSwapBuffer = clAlloc(sizeof(real) * volume / localSize[0]);
	
	dtBuffer = clAlloc(sizeof(real16));
	stateBuffer = clAlloc(sizeof(real) * numStates() * volume);
	
	//get the edges, so reduction doesn't
	{
		std::vector<real> cflVec(volume);
		for (real &r : cflVec) { r = std::numeric_limits<real>::max(); }
		commands.enqueueWriteBuffer(cflBuffer, CL_TRUE, 0, sizeof(real) * volume, &cflVec[0]);
	}
	
	commands.enqueueWriteBuffer(dtBuffer, CL_TRUE, 0, sizeof(real), &app->fixedDT);

	vectorField = std::make_shared<HydroGPU::Plot::VectorField>(this);
	
	switch(app->dim) {
	case 1:
		plot = std::make_shared<HydroGPU::Plot::Plot1D>(this);
		break;
	case 2:
		plot = std::make_shared<HydroGPU::Plot::Plot2D>(this);
		break;
	case 3:
		plot = std::make_shared<HydroGPU::Plot::Plot3D>(this);
		break;
	}
	
	fluidTexMem = cl::ImageGL(app->context, CL_MEM_WRITE_ONLY, GL_TEXTURE_3D, 0, plot->fluidTex);
}

static std::string boundaryKernelNames[NUM_BOUNDARY_KERNELS] = {
	"Periodic",
	"Mirror",
	"Reflect",
	"FreeFlow"
};

void Solver::initKernels() {
	
	int volume = getVolume();

	boundaryKernels.resize(NUM_BOUNDARY_KERNELS);
	for (std::vector<cl::Kernel>& v : boundaryKernels) {
		v.resize(app->dim);
	}

	std::vector<std::string> dimNames = {"X", "Y", "Z"};
	for (int boundaryIndex = 0; boundaryIndex < NUM_BOUNDARY_KERNELS; ++boundaryIndex) {
		for (int side = 0; side < app->dim; ++side) {
			std::string name = "stateBoundary" + boundaryKernelNames[boundaryIndex] + dimNames[side];
			boundaryKernels[boundaryIndex][side] = cl::Kernel(program, name.c_str());
			app->setArgs(boundaryKernels[boundaryIndex][side], stateBuffer);
		}
	}
	
	calcCFLMinReduceKernel = cl::Kernel(program, "calcCFLMinReduce");
	app->setArgs(calcCFLMinReduceKernel, cflBuffer, cl::Local(localSize[0] * sizeof(real)), volume, cflSwapBuffer);
	
	convertToTexKernel = cl::Kernel(program, "convertToTex");
	app->setArgs(convertToTexKernel, stateBuffer, fluidTexMem, app->gradientTexMem);
}

std::vector<std::string> Solver::getProgramSources() {
	std::vector<std::string> sourceStrs = std::vector<std::string>{
		std::string() +
		"#define DIM " + std::to_string(app->dim) + "\n" +
		"#define SIZE_X " + std::to_string(app->size.s[0]) + "\n" +
		"#define SIZE_Y " + std::to_string(app->size.s[1]) + "\n" +
		"#define SIZE_Z " + std::to_string(app->size.s[2]) + "\n" +
		"#define STEP_X 1\n" +
		"#define STEP_Y " + std::to_string(app->size.s[0]) + "\n" +
		"#define STEP_Z " + std::to_string(app->size.s[0] * app->size.s[1]) + "\n" +
		"#define STEP_W " + std::to_string(app->size.s[0] * app->size.s[1] * app->size.s[2]) + "\n" +
		"#define DX " + toNumericString<real>(app->dx.s[0]) + "\n" +
		"#define DY " + toNumericString<real>(app->dx.s[1]) + "\n" +
		"#define DZ " + toNumericString<real>(app->dx.s[2]) + "\n" +
		"#define XMIN " + toNumericString<real>(app->xmin.s[0]) + "\n" +
		"#define YMIN " + toNumericString<real>(app->xmin.s[1]) + "\n" +
		"#define ZMIN " + toNumericString<real>(app->xmin.s[2]) + "\n" +
		"#define XMAX " + toNumericString<real>(app->xmax.s[0]) + "\n" +
		"#define YMAX " + toNumericString<real>(app->xmax.s[1]) + "\n" +
		"#define ZMAX " + toNumericString<real>(app->xmax.s[2]) + "\n" +
		"#define NUM_STATES " + std::to_string(numStates()) + "\n"
	};

	std::string slopeLimiterName = "Superbee";
	app->lua.ref()["slopeLimiter"] >> slopeLimiterName;
	sourceStrs[0] += "#define SLOPE_LIMITER_" + slopeLimiterName + "\n";

	real gravitationalConstant = 1.f;
	app->lua.ref()["gravitationalConstant"] >> gravitationalConstant;
	sourceStrs[0] += "#define GRAVITATIONAL_CONSTANT " + toNumericString<real>(gravitationalConstant) + "\n";

	sourceStrs.push_back(Common::File::read("SlopeLimiter.cl"));
	sourceStrs.push_back(Common::File::read("Common.cl"));
	equation->getProgramSources(sourceStrs);
	return sourceStrs;
}

Solver::ResetStateContext::ResetStateContext(int volume, int numStates)
: stateVec(volume*numStates)
{}

std::shared_ptr<Solver::ResetStateContext> Solver::createResetStateContext() {
	return std::make_shared<ResetStateContext>(getVolume(), numStates());
}

void Solver::resetStateCell(std::shared_ptr<Solver::ResetStateContext> ctx, int index, const std::vector<real>& cellResults) {
	equation->readStateCell(&ctx->stateVec[index*numStates()], cellResults.data());
}

void Solver::resetStateDone(std::shared_ptr<Solver::ResetStateContext> ctx) {
	//write state density first for gravity potential, to then update energy
	commands.enqueueWriteBuffer(stateBuffer, CL_TRUE, 0, sizeof(real) * numStates() * getVolume(), ctx->stateVec.data());
	commands.finish();
}

void Solver::resetState() {
	std::shared_ptr<ResetStateContext> ctx = createResetStateContext();

	if (!app->lua.ref()["initState"].isFunction()) throw Common::Exception() << "expected initState to be defined in config file";
	std::cout << "initializing..." << std::endl;

	//hard-coded and centered around the MHD solver *and* the extra pressure buffer for Euler/MHD equations ...
	//TODO multret and have each equation interpret the results
	std::vector<real> cellResults(9);

	int flattenedIndex = 0;
	int index[3];
	for (index[2] = 0; index[2] < app->size.s[2]; ++index[2]) {
		for (index[1] = 0; index[1] < app->size.s[1]; ++index[1]) {
			for (index[0] = 0; index[0] < app->size.s[0]; ++index[0], ++flattenedIndex ) {
				real4 pos;
				for (int i = 0; i < 3; ++i) {
					pos.s[i] = real(app->xmax.s[i] - app->xmin.s[i]) * (real(index[i]) + .5) / real(app->size.s[i]) + real(app->xmin.s[i]);
				}
				pos.s[3] = 0;
			
				LuaCxx::Stack stack = app->lua.stack();
				
				stack
				.getGlobal("initState")
				.push(pos.s[0], pos.s[1], pos.s[2])
				.call(3, cellResults.size());	
				
				for (int i = cellResults.size()-1; i >= 0; --i) {
					cellResults[i] = real();
					stack.pop(cellResults[i]);
				}
				
				resetStateCell(ctx, flattenedIndex, cellResults);
			}
		}
	}
	std::cout << "...done" << std::endl;

	//grad^2 Phi = - 4 pi G rho
	//solve inverse discretized linear system to find Psi
	//D_ij / (-4 pi G) Phi_j = rho_i
	//once you get that, plug it into the total energy

	//pass context to child class 
	resetStateDone(ctx);
}

int Solver::numStates() {
	return (int)equation->states.size();
}

int Solver::getVolume() {
	return app->size.s[0] * app->size.s[1] * app->size.s[2];
}

void Solver::getBoundaryRanges(int dimIndex, cl::NDRange &offset, cl::NDRange &global, cl::NDRange &local) {
	switch (app->dim) {
	case 1:
	case 2:
		offset = offset1d;
		local = localSize1d;
		global = cl::NDRange(app->size.s[dimIndex]);
		break;
	case 3:
		offset = cl::NDRange(0, 0);
		local = cl::NDRange(localSize[0], localSize[1]);
		switch (dimIndex) {
		case 0:
			global = cl::NDRange(app->size.s[0], app->size.s[1]);
			break;
		case 1:
			global = cl::NDRange(app->size.s[0], app->size.s[2]);
			break;
		case 2:
			global = cl::NDRange(app->size.s[1], app->size.s[2]);
			break;
		default:
			throw Common::Exception() << "can't handle dim " << dimIndex;
		}
		break;
	default:
		throw Common::Exception() << "can't handle dim " << dimIndex;
	}
}

void Solver::boundary() {
	cl::NDRange offset, global, local;
	for (int i = 0; i < app->dim; ++i) {
		getBoundaryRanges(i, offset, global, local);
		for (int j = 0; j < numStates(); ++j) {
			int boundaryKernelIndex = equation->stateGetBoundaryKernelForBoundaryMethod(i, j);
			cl::Kernel& kernel = boundaryKernels[boundaryKernelIndex][i];
			app->setArgs(kernel, stateBuffer, numStates(), j);
			commands.enqueueNDRangeKernel(kernel, offset, global, local);
		}
	}
}

void Solver::findMinTimestep() {
	int reduceSize = getVolume();
	cl::Buffer dst = cflSwapBuffer;
	cl::Buffer src = cflBuffer;
	while (reduceSize > 1) {
		int nextSize = (reduceSize >> 4) + !!(reduceSize & ((1 << 4) - 1));
		cl::NDRange reduceGlobalSize(std::max<int>(reduceSize, localSize[0]));
		calcCFLMinReduceKernel.setArg(0, src);
		calcCFLMinReduceKernel.setArg(2, reduceSize);
		calcCFLMinReduceKernel.setArg(3, nextSize == 1 ? dtBuffer : dst);
		commands.enqueueNDRangeKernel(calcCFLMinReduceKernel, offset1d, reduceGlobalSize, localSize1d);
		if (app->useGPU) commands.finish();
		std::swap(dst, src);
		reduceSize = nextSize;
	}
}

void Solver::initStep() {
}

void Solver::update() {
	if (app->showTimestep) {
		real dt;
		commands.enqueueReadBuffer(dtBuffer, CL_TRUE, 0, sizeof(real), &dt);
		std::cout << "dt " << dt << std::endl;
	}

	//commands.enqueueNDRangeKernel(addSourceKernel, offsetNd, globalSize, localSize, nullptr, &addSourceEvent.clEvent);

	boundary();
	
	initStep();
	
	if (!app->useFixedDT) {
		calcTimestep();
	}

	step();

/*
	for (EventProfileEntry *entry : entries) {
		cl_ulong start = entry->clEvent.getProfilingInfo<CL_PROFILING_COMMAND_START>();
		cl_ulong end = entry->clEvent.getProfilingInfo<CL_PROFILING_COMMAND_END>();
		entry->stat.accum((double)(end - start) * 1e-9);
	}
*/

}

void Solver::display() {
	glFinish();
	
	std::vector<cl::Memory> acquireGLMems = {fluidTexMem};
	commands.enqueueAcquireGLObjects(&acquireGLMems);

	//TODO if we're not using GPU then we need to transfer the contents via a CPU buffer ... or not at all?
	if (app->useGPU) {
		convertToTexKernel.setArg(3, app->displayMethod);
		convertToTexKernel.setArg(4, app->displayScale);
		commands.enqueueNDRangeKernel(convertToTexKernel, offsetNd, globalSize, localSize);
	}

	commands.enqueueReleaseGLObjects(&acquireGLMems);
	commands.finish();

	plot->display();	
	vectorField->display();
	
	{int err = glGetError();
	if (err) std::cout << "GL error " << err << " at " << __FILE__ << ":" << __LINE__ << std::endl;}
}

void Solver::resize() {
	plot->resize();
}

void Solver::mouseMove(int x, int y, int dx, int dy) {
}

void Solver::mousePan(int dx, int dy) {
	plot->mousePan(dx, dy);
}

void Solver::mouseZoom(int dz) {
	plot->mouseZoom(dz);
}

void Solver::addDrop() {
}

void Solver::screenshot() {
	for (int i = 0; i < 1000; ++i) {
		std::string filename = std::string("screenshot") + std::to_string(i) + "layer0.png";
		if (!Common::File::exists(filename)) {
			plot->screenshot(filename);
			return;
		}
	}
	throw Common::Exception() << "couldn't find an available filename";
}

void Solver::save() {
	std::vector<std::string> channelNames = equation->states;
	channelNames.push_back("potential");

	for (int i = 0; i < 1000; ++i) {
		std::string filename = channelNames[0] + std::to_string(i) + ".fits";
		if (!Common::File::exists(filename)) {
			
			//hmm, rather than a plane per variable, now that I'm saving 3D stuff,
			// how about a plane per 3rd dim, and separate save files per variable?
			std::shared_ptr<Image::ImageType<float>> image = std::make_shared<Image::ImageType<float>>(Tensor::Vector<int,2>(app->size.s[0], app->size.s[1]), nullptr, 1, app->size.s[2]);

			int volume = getVolume();
			
			std::vector<real> stateVec(numStates() * volume);
			app->commands.enqueueReadBuffer(stateBuffer, CL_TRUE, 0, sizeof(real) * numStates() * volume, &stateVec[0]);

//TODO re-enable saving of potential buffer.  separate filename choosing from writing, and overload the write function 
//			std::vector<real> potentialVec(volume);
//			app->commands.enqueueReadBuffer(selfgrav->potentialBuffer, CL_TRUE, 0, sizeof(real) * volume, &potentialVec[0]);
			
			app->commands.finish();
			
			for (int channel = 0; channel < channelNames.size(); ++channel) {
				for (int z = 0; z < app->size.s[2]; ++z) {	
					for (int y = 0; y < app->size.s[1]; ++y) {
						for (int x = 0; x < app->size.s[0]; ++x) {
							int index = x + app->size.s[0] * (y + app->size.s[1] * z);
							real value = std::nan("");	
							if (channel < numStates()) {
								value = stateVec[channel + numStates() * index];
//							} else {	//potential
//								value = potentialVec[index];
							}
							(*image)(x,y,0,z) = value;
						}
					}
				}
				std::string filename = channelNames[channel] + std::to_string(i) + ".fits";
				std::cout << "saving file " << filename << std::endl;
				Image::system->write(filename, image); 
			}
			return;
		}
	}
}



}
}

