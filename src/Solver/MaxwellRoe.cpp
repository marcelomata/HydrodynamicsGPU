#include "HydroGPU/Solver/MaxwellRoe.h"
#include "HydroGPU/HydroGPUApp.h"
#include "HydroGPU/Equation/Maxwell.h"

namespace HydroGPU {
namespace Solver {

void MaxwellRoe::initKernels() {
	Super::initKernels();
	
	addSourceKernel = cl::Kernel(program, "addSource");
	addSourceKernel.setArg(1, stateBuffer);
}
	
void MaxwellRoe::createEquation() {
	equation = std::make_shared<HydroGPU::Equation::Maxwell>(app);
}

std::vector<std::string> MaxwellRoe::getProgramSources() {
	std::vector<std::string> sources = Super::getProgramSources();
	sources.push_back("#include \"MaxwellRoe.cl\"\n");
	return sources;
}

//zero cell-based information required.
// unless you want to store permittivity and permeability.
// would a dynamic permittivity and permeability affect the flux equations?
int MaxwellRoe::getEigenTransformStructSize() {
	//how will OpenCL respond to an allocation of zero bytes?
	//not well...
	return 1;
}

std::vector<std::string> MaxwellRoe::getEigenProgramSources() {
	return {};
}

void MaxwellRoe::step(real dt) {
	Super::step(dt);
	
	//see ADM1DRoe::step() for my thoughts on source and separabe integration
	integrator->integrate(dt, [&](cl::Buffer derivBuffer) {
		addSourceKernel.setArg(0, derivBuffer);
		commands.enqueueNDRangeKernel(addSourceKernel, offsetNd, globalSize, localSize);
	});
}

}
}
