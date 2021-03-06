#include "HydroGPU/Solver/ADM3DRoe.h"
#include "HydroGPU/HydroGPUApp.h"
#include "HydroGPU/Equation/ADM3D.h"

namespace HydroGPU {
namespace Solver {

void ADM3DRoe::createEquation() {
	equation = std::make_shared<HydroGPU::Equation::ADM3D>(app);
}

void ADM3DRoe::initKernels() {
	Super::initKernels();
	
	addSourceKernel = cl::Kernel(program, "addSource");
	addSourceKernel.setArg(1, stateBuffer);

	constrainKernel = cl::Kernel(program, "constrain");
	constrainKernel.setArg(0, stateBuffer);
}

std::vector<std::string> ADM3DRoe::getProgramSources() {
	std::vector<std::string> sources = Super::getProgramSources();
	sources.push_back("#include \"ADM3DRoe.cl\"\n");
	return sources;
}

std::vector<std::string> ADM3DRoe::getCalcFluxDerivProgramSources() {
	return {};
}

std::vector<std::string> ADM3DRoe::getEigenProgramSources() {
	return {};
}

int ADM3DRoe::getEigenTransformStructSize() {
	return 7 + 30 + 6 + 1 + 1;	//time states, field states, gInv, g, f
}

int ADM3DRoe::getEigenSpaceDim() {
	return 30;	//37 state variables, but skip the first 7 (which only have source terms and which aren't used in any eigenfields)
}

void ADM3DRoe::step(real dt) {
#if 0
//debug output
std::vector<real> stateVec(numStates() * getVolume());
commands.enqueueReadBuffer(stateBuffer, CL_TRUE, 0, sizeof(real) * numStates() * getVolume(), stateVec.data());
for (int i = 0; i < getVolume(); ++i) {
	for (int j = 0; j < numStates(); ++j) {
		printf("\t%f", stateVec[j + numStates() * i]);
	}
}
#endif

	//advect
	Super::step(dt);

	//see ADM1DRoe::step() for my thoughts on source and separabe integration
	//in fact, now that this is separated, it doesn't seem to be as stable ...
	integrator->integrate(dt, [&](cl::Buffer derivBuffer) {
		addSourceKernel.setArg(0, derivBuffer);
		commands.enqueueNDRangeKernel(addSourceKernel, offsetNd, globalSize, localSize);
	});

	commands.enqueueNDRangeKernel(constrainKernel, offsetNd, globalSize, localSize);
}

}
}
