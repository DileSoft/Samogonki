#ifndef __UNKNOWN_H__
#define __UNKNOWN_H__

#include "assert.h"

#define UNKNOWN_ASSERT(a)											assert(a)
#define UNKNOWN_TYPE(a)												((a)&0x000000FF)
#define UNKNOWN_DESTRUCTOR											cUnknownClass::Release(); if(cUnknownClass::GetRef()) Release();
extern unsigned int GlobalUniqueCounter;

enum eKindUnknownClass
{	// unsigned short
	KIND_NULL				=		0,
//	1	..	7	- ���������� �������
	KIND_UI_VISGENERIC		=		1,				// ������� ���������� ������������
	KIND_UI_TEXTURE			=		2,				// ��������� ��� ������ � ����������
	KIND_UI_OBJECT			=		3,				// ��������� ��� ������ � ���������
//	8	..	15	- ������ ��� ������ � ����������
	KIND_TEX_BITMAP			=		8,				// ������� ����� �������� cTextureBitMap 
	KIND_TEX_MIPMAP			=		9,				// ����� �������� cUnknownLOD - ��������� ����������
	KIND_TEX_ANIMATION		=		10,				// ����� �������� cTextureAnimation �� ���������
//	16	..	23	- ������ ��� ������ � ���������
	KIND_OBJ_LOD			=		16,				// cObjectLOD - ��������� ������� �����������
	KIND_OBJ_MATERIAL		=		17,				// cObjectMaterial - ����� ��������� �������
	KIND_OBJ_NODE			=		18,				// cObjectNode - ����������� �����-������ ��� ���� ��������
	KIND_OBJ_ANIMATION		=		19,				// cNodeAnimation - ����� ������� ������ ��������
//	24	..	31	- ������������������ ������ �������� Mesh - �������������� ���� 
	KIND_MESH_NODE			=		24,				// cMeshNode - ���������� ���������� �������������� ������� Mesh'�
	KIND_MESH_MAPPING		=		25,				// cMeshMapping - ����� �������� ������ ��������� 
//	KIND_MESH_TILE			=		26,				// cMeshTile - ����� �������� �������
//	32	..	63	- ������ ������ 
	KIND_SCENE				=		32,				// cScene - ����� ����� 
	KIND_CAMERA				=		33,				// cCamera - ����� ������
	KIND_MESH				=		34,				// cMesh - ����� ��������
	KIND_WORLD_TILEMAP		=		35,				// cTileMap	- ����� �������� ������������� �����
	KIND_WORLD_TILEWATER	=		36,				// cTileWater - ����� �������� ������������� �����
	KIND_RENDERDEVICE		=		37,				// cRenderDevice - ���������� ��� cInterfaceGraph3d
	KIND_ARRAYCAMERA		=		38,				// cUnkClassDynArrayPointer - ����� ���������� cCamera
	KIND_ARRAYSCENE			=		39,				// cUnkClassDynArrayPointer - ����� ���������� cScene
	KIND_ARRAYSUN			=		40,				// cUnkClassDynArrayPointer - ����� ���������� cSun
//	64	..	128	- ������ ����������
	KIND_LIB_TEXTURE		=		64,				// cTextureLibrary 
	KIND_LIB_OBJECT			=		65,				// cObjectLibrary
};
enum eTypeUnknownClass
{	// unsigned short
	TYPE_NULL				=		0,
// bit 1 - bit 31 ����� �������
	TYPE_POINTER			=		1<<0,			// ����� ������������ ��� ������� ����������
	TYPE_DELETE				=		1<<31			// ������� ����, ��� ����� ������
};

// ������� ����� ��� ����
// ����� ����� ������������� ��� TYPE_CLASS_POINTER, ������ ����� ��������� �� ��������� � Release()
class cUnknownClass
{	
	int					m_cRef;
	unsigned int		Unique;
	eKindUnknownClass	Kind;
	eTypeUnknownClass	Type;
public:
	cUnknownClass(int kind=KIND_NULL,int type=TYPE_NULL)
	{ 
		m_cRef=1; 
		Kind=eKindUnknownClass(kind);
		Type=eTypeUnknownClass(type);
		Unique=++GlobalUniqueCounter; 
		UNKNOWN_ASSERT(Unique); 
	}
	~cUnknownClass()								
	{ 
		UNKNOWN_ASSERT(GetType(TYPE_DELETE)); 
		if(GetType(TYPE_POINTER))
			UNKNOWN_ASSERT(m_cRef==0); 
	}
	inline int GetKind(int kind)									{ return Kind==kind; }
	inline int GetKind()											{ return Kind; }
	inline int GetType(int type)									{ return Type&type; }
	inline int GetType()											{ return Type; }
	inline int SetType(int type)									{ return Type=eTypeUnknownClass(Type|type); }
	inline int GetRef()												{ return m_cRef; }
	inline int IncRef()												{ return ++m_cRef; }
	inline int DecRef()												{ return --m_cRef; }
	inline int GetUnique()											{ return Unique; }
	inline int Release()
	{
		SetType(TYPE_DELETE);
		return m_cRef;
	}
};

#define UI_BEGIN(UI_Class,UI_Kind)									class UI_Class : public cUnknownInterface { public: UI_Class() : cUnknownInterface(UI_Kind) {} ~UI_Class()	{ UNKNOWN_ASSERT(cUnknownClass::GetRef()==0); }
#define UI_END(UI_Class)											};
#ifndef UI_IMPORT
#define UI_IMPORT(UI_Function)										virtual UI_Function=0
#endif

class cUnknownInterface : public cUnknownClass
{
public:
	cUnknownInterface(int kind,int type=TYPE_POINTER) : cUnknownClass(kind,type)	{ }
	~cUnknownInterface()										
	{ 
		cUnknownClass::Release(); 
	}
	virtual int Release()											{ delete this; return 0; }
	virtual void Delete(cUnknownClass *UnknownClass)				{ assert(UnknownClass==0); }
};

#endif //__UNKNOWN_H__