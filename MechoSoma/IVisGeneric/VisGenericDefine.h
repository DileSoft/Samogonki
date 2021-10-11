#ifndef __VISGENERICDEFINE_H__
#define __VISGENERICDEFINE_H__

enum eAttributeCamera
{
	ATTRIBUTE_CAMERA_NULL								=	0,
	ATTRIBUTE_CAMERA_PERSPECTIVE						=	1<<0,
	ATTRIBUTE_CAMERA_WORLD_SHARE						=	1<<1,
	ATTRIBUTE_CAMERA_WORLD_CUTTING						=	1<<2,

	ATTRIBUTE_CAMERA_PERSPECTIVE_WORLD_SHARE			=	ATTRIBUTE_CAMERA_PERSPECTIVE | ATTRIBUTE_CAMERA_WORLD_SHARE,
	ATTRIBUTE_CAMERA_PERSPECTIVE_WORLD_CUTTING			=	ATTRIBUTE_CAMERA_PERSPECTIVE | ATTRIBUTE_CAMERA_WORLD_CUTTING,
	ATTRIBUTE_CAMERA_PERSPECTIVE_WORLD_SHARE_CUTTING	=	ATTRIBUTE_CAMERA_PERSPECTIVE | ATTRIBUTE_CAMERA_WORLD_SHARE | ATTRIBUTE_CAMERA_WORLD_CUTTING,
	// 28..29
	ATTRIBUTE_CAMERA_MASK_A								=	1<<27,
	ATTRIBUTE_CAMERA_MASK_B								=	1<<28,
};

enum eConstVisible
{ // ��������� ��������� �� ��������� � �������� ���������, ��������� � �����
	CONST_VISIBLE_NULL									=	0,		
	CONST_VISIBLE_SHOW									=	1<<0,	// ������ ���������� (��������)
	CONST_VISIBLE_XMIN									=	1<<1,	// �����, ��� ����������� � ����� ���������� �������� ���������
	CONST_VISIBLE_XMAX									=	1<<2,	// �����, ��� ����������� � ������ ���������� �������� ���������
	CONST_VISIBLE_YMIN									=	1<<3,	// �����, ��� ����������� � ������� ���������� �������� ���������
	CONST_VISIBLE_YMAX									=	1<<4,	// �����, ��� ����������� � ������ ���������� �������� ���������
	CONST_VISIBLE_ZMIN									=	1<<5,	// �����, ��� ����������� � ������� ���������� �������� ���������
	CONST_VISIBLE_ZMAX									=	1<<6,	// �����, ��� ����������� � ������� ���������� �������� ���������
	CONST_VISIBLE_FRUSTUM								= CONST_VISIBLE_XMIN | CONST_VISIBLE_XMAX | CONST_VISIBLE_YMIN | CONST_VISIBLE_YMAX | CONST_VISIBLE_ZMIN | CONST_VISIBLE_ZMAX,
};

// ������ ����������� ��� �������������, � �������� ����������� ������ ���� �������
#define CONST_VISIBLE									1

#endif __VISGENERICDEFINE_H__