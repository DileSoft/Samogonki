#ifndef __MANAGERTEXTURE_H__
#define __MANAGERTEXTURE_H__

#include "Texture.h"
#include "ITexture.h"

class cManagerTexture : public cInterfaceTexture
{
public:
	// ���������� cInterfaceTexture 
	cManagerTexture();
	~cManagerTexture();
	virtual int Release();
	virtual void Delete(cUnknownClass* Texture);								// �������� ����� ��������

	virtual cUnknownClass* Get(char *NameTexture,char *NameOpacity=0);			// �������� ������������� �������� cTextureAnimation
	virtual void Free();														// ������������ �������������� �������

protected:
	cUnknownLibrary			TextureLibrary;
	// ������� ��� ������ � ����������� �������
	cUnknownClass* GetTextureAnimation(char *NameTexture,char *NameOpacity=0);	// ���������� cTextureAnimation
	void DeleteTexture(cUnknownClass *Texture);									// ��� �������� �������, ���������� �����������
};

#endif //__MANAGERTEXTURE_H__