#include "MgrTex.h"

cManagerTexture::cManagerTexture() : TextureLibrary(KIND_LIB_TEXTURE)
{
}
cManagerTexture::~cManagerTexture()
{
}

////////////////////////////// INTERFACE //////////////////////////////
int cManagerTexture::Release()
{
	if(cUnknownInterface::DecRef()) return cUnknownInterface::GetRef();
	this->Free();
	delete this;
	return 0;
}
void cManagerTexture::Delete(cUnknownClass *Texture)						
{ 
	assert(Texture);
	this->DeleteTexture(Texture);
}
cUnknownClass* cManagerTexture::Get(char *NameTexture,char *NameOpacity)
{	// �������� ������������� �������� cTextureAnimation
	return this->GetTextureAnimation(NameTexture,NameOpacity);
}
void cManagerTexture::Free()
{	// ������������ ������ �� �������������� �������
	for(cUnknownClassList *tmp=TextureLibrary.BaseList,*start=TextureLibrary.BaseList;tmp;tmp=start)
	{
		start=start->next;
		if(tmp->Base->GetRef()==1) // �� ������ ������ ������� ������������ ������ tmp->Base
		{
			DeleteTexture(tmp->Base);
			tmp->Base=0;
			TextureLibrary.Detach(tmp);
		}
	}
}
////////////////////////////// TextureLibrary //////////////////////////////
void cManagerTexture::DeleteTexture(cUnknownClass *Texture)
{
	switch(Texture->GetKind())
	{
		case KIND_TEX_ANIMATION:
			((cTextureAnimation*)Texture)->Release();
			break;
		default:
			assert(0);
	}
}
cUnknownClass* cManagerTexture::GetTextureAnimation(char *NameTexture,char *NameOpacity)
{	// ������� ��� ������ ������ � �������������� ���������� sAnimationTexture
	assert(NameTexture&&strlen(NameTexture));
	for(cUnknownClassList *List=TextureLibrary.BaseList;List;List=List->next)
		if(List->Base->GetKind(KIND_TEX_ANIMATION))
		{
			cTextureAnimation *TexAnim=(cTextureAnimation*)List->Base;
			if((TexAnim->NameTexture==NameTexture)&&(TexAnim->NameOpacity==NameOpacity))
			{ 
				TexAnim->IncRef(); // ��������� ������� ��� ����, ��� ��������
				return TexAnim; 
			}
		}
	// ������� �������� � ���������� �� �������, ��������� ��
	cTextureAnimation *TexAnim=new cTextureAnimation;
	TextureLibrary.Attach(TexAnim);
	TexAnim->NameTexture=NameTexture;
	TexAnim->NameOpacity=NameOpacity;
	TexAnim->Load();
	return TexAnim;
}
