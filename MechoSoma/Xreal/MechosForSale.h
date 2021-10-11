////////////////////////////////////////////////////////////////////
//		Mechos ��� ��������
////////////////////////////////////////////////////////////////////
#include "Body.h"
#include "Mesh3ds.h"

class MechosForSale : Body {
	// ������ ��� ��������� � ������� ������� (0-engine, front, back, rfw, rbw, lfw, lbw)
	static int PART_INDEX(int type)	{ return BitSR(M3D_TYPE(type)); }
	// ��������� � ������ �� ����
	cMesh* part_by_type(int partType) const;
	// ��������� � ������ �� �������: 0-engine, 1-front, 2-back, 3-rf, 4-rb, 5-lf, 6-lb
	cMesh* part_by_index(int index) const;

public:
	MechosForSale(char* KindsStr, const Vect3f& pos);
	void changePart(int part_index, int part_number);
	void changePart(cMesh* part);
	void rotate(int dx, int dy);
	char* getConfig(); // allocates memory by new()
	cMesh* getGeometry() { return geometry; }
	cMesh* getPart(int id) { return part_by_type(id); }
};			

