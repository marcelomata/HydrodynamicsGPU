#pragma once

#include "HydroGPU/Solver/Roe.h"

namespace HydroGPU {
namespace Solver {

/*
Roe solver for ADM3D equations
*/
struct ADM3DRoe : public Roe {
protected:
	typedef Roe Super;
	
	cl::Kernel addSourceKernel;

public:
	using Super::Super;
	virtual void init();

protected:
	virtual void createEquation();
	virtual std::vector<std::string> getProgramSources();
	virtual void calcDeriv(cl::Buffer derivBuffer);
};

}
}

