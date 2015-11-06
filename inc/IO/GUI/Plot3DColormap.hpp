#ifndef PLOT2D_HPP
#define PLOT2D_HPP

#include "IO/GUI/GUIElement.hpp"
#include "IO/Display.hpp"

namespace vfps
{

class Plot3DColormap : public GUIElement
{
public:
	Plot3DColormap();

	~Plot3DColormap();

	void createTexture(PhaseSpace* mesh);

	void delTexture();

	void draw();
};

} // namespace vfps

#endif // PLOT2D_HPP