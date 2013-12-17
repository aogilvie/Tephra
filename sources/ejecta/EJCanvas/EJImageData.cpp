#include "EJImageData.h"

EJImageData::EJImageData(int widthp, int heightp, GLubyte * pixelsp):
width(widthp), height(heightp), pixels(pixelsp), m_texture(NULL) {
	//- (EJTexture *)texture {
	//	return [[[EJTexture alloc] initWithWidth:width height:height pixels:pixels] autorelease];
	//}
    //texture();
}

EJImageData::~EJImageData()
{
	free(pixels);
}

EJTexture* EJImageData::texture() {
	m_texture = new EJTexture(width, height, pixels);
	m_texture->autorelease();
	return m_texture;
}