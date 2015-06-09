#include "HydroGPU/Solver/MHDRoe.h"
#include "HydroGPU/Equation/MHD.h"
#include "HydroGPU/HydroGPUApp.h"

namespace HydroGPU {
namespace Solver {

void MHDRoe::initBuffers() {
	Super::initBuffers();
	
	//allocate flux flag buffer for determining if any flux values had to be pre-filled for bad eigenstate areas
	fluxFlagBuffer = clAlloc(sizeof(char) * getVolume() * app->dim);
}

void MHDRoe::initKernels() {
	Super::initKernels();

	//all Euler and MHD systems also have a separate potential buffer...
	app->setArgs(calcEigenBasisKernel, eigenvaluesBuffer, eigenfieldsBuffer, stateBuffer, selfgrav->potentialBuffer, fluxBuffer, fluxFlagBuffer);

	//just like ordinary calcMHDFluxKernel -- and calls the ordinary
	// -- but with an extra step to bail out of the associated fluxFlag is already set 
	calcMHDFluxKernel = cl::Kernel(program, "calcMHDFlux");
	app->setArgs(calcMHDFluxKernel, fluxBuffer, stateBuffer, eigenvaluesBuffer, eigenfieldsBuffer, deltaQTildeBuffer, 0, 0, fluxFlagBuffer);
}
	
void MHDRoe::createEquation() {
	equation = std::make_shared<HydroGPU::Equation::MHD>(this);
}

std::vector<std::string> MHDRoe::getProgramSources() {
	std::vector<std::string> sources = Super::getProgramSources();
	sources.push_back("#include \"MHDRoe.cl\"\n");
	return sources;
}

void MHDRoe::initStep() {
	//MHD-Roe is special in that it can write fluxes during the calc eigen basis kernel
	//(in the case of negative fluxes)
	// so for that, I'm going to fill the flux kernel to some flag beforehand.
	// zero is a safe flag, right?  no ... not for steady states ...
	commands.enqueueFillBuffer(fluxFlagBuffer, 0, 0, getVolume() * app->dim);
	
	//and fill buffer
	Super::initStep();
}

//override parent call
//call this instead
//it'll call through the CL code if it's needed
void MHDRoe::calcFlux(int side) {
	calcMHDFluxKernel.setArg(5, dt);
	calcMHDFluxKernel.setArg(6, side);
	commands.enqueueNDRangeKernel(calcMHDFluxKernel, offsetNd, globalSize, localSize);
}

void MHDRoe::step() {
	Super::step();
	selfgrav->applyPotential();
	divfree->update();
}

}
}

