#ifndef __FRAME_H__
#define __FRAME_H__

#include "BaseClass.h"
#include "cString.h"
#include "UMATH.H"

#define KFRAME_COS				(1<<0)
#define KFRAME_SLERP			(1<<1)
#define KFRAME_LINEAR			(1<<2)
#define KFRAME_QUADRATIC		(1<<3)
#define KFRAME_ARCLENGTH		(1<<4)

#define M3D_DEFAULT_ANIMATION_PERIOD	2000

const float frame_phase_range = 1.00001f; // in order to SetPhase(0) != SetPhase(1)

enum eAttributeAnimTri
{
	ANIMCHAIN_NULL					=	0,
	ANIMCHAIN_CTILE					=	1<<0,	// key=cTile
	ANIMCHAIN_COPY					=	1<<1,	// ������ ������������ ������� �������� �������
	ANIMCHAIN_CYCL					=	1<<2,	// ������������ ������� �����������
};

class cTile;
class cMesh;
struct sAnimMaterial;

struct sKeyTri
{ //���� ������������ ���� ��� ����������� �������
	cTile						*Key;			// ������������ ���������
	float						Time;			// ��������������� ����� � �������������

	sKeyTri()														{ Key=0; Time=0; }
	~sKeyTri();														
	inline void set(cTile *key,float time)							{ Key=key; Time=time; }
	inline sKeyTri& operator = (sKeyTri &AnimKey)					{ Key=AnimKey.Key; Time=AnimKey.Time; AnimKey.set(0,0);	return *this; }
};
struct sKeyMaterial
{ //���� ������������ ���� ��� ����������� �������
	sAnimMaterial				*Key;			// ������������ ���������
	float						Time;			// ��������������� ����� � �������������

	sKeyMaterial()													{ Key=0; Time=0; }
	~sKeyMaterial();														
	inline void set(sAnimMaterial *key,float time)					{ Key=key; Time=time; }
	inline sKeyMaterial& operator = (sKeyMaterial &AnimKey)			{ Key=AnimKey.Key; Time=AnimKey.Time; AnimKey.set(0,0);	return *this; }
};
class cAnimChain : public sAttribute // ������� ����������� ������������ �������
{ //���� ������������ �������
	cBaseDynArray<sKeyTri>		KeyTri;
	cBaseDynArray<sKeyMaterial>	KeyMaterial;
	float						TimeChain;		// ����������������� ������������ ������� � �������������
	cString						name;			// ��� ������������ �������
public:
	cAnimChain();
	cAnimChain(cAnimChain *AnimChain);
	~cAnimChain();
	void GetTile(cTile *Tile,float phase);	// ���������� NewKey � ������������ � phase
	inline int GetNumberTri()									{ return KeyTri.length(); }
	inline sKeyTri* GetTri(int number)							{ assert(0<=number&&number<GetNumberTri()); return &KeyTri[number]; }
	inline sKeyTri* AddTri()									{ KeyTri.Resize(GetNumberTri()+1); return GetTri(GetNumberTri()-1); }
	inline int GetTri(float phase)								
	{ 
		float time=phase*TimeChain;
		for(int i=0;i<GetNumberTri();i++) 
			if(time<=GetTri(i)->Time) 
				return i; 
			return GetNumberTri()-1; 
	}
	inline int GetNumberMaterial()								{ return KeyMaterial.length(); }
	inline sKeyMaterial* GetMaterial(int number)				{ assert(0<=number&&number<GetNumberMaterial()); return &KeyMaterial[number]; }
	inline sKeyMaterial* AddMaterial()							{ KeyMaterial.Resize(GetNumberMaterial()+1); return GetMaterial(GetNumberMaterial()-1); }
	inline int GetMaterial(float phase)								
	{ 
		float time=phase*TimeChain;
		for(int i=0;i<GetNumberMaterial();i++) 
			if(time<=GetMaterial(i)->Time) 
				return i; 
			return GetNumberMaterial()-1; 
	}
	inline void SetTimeChain(float time)							{ TimeChain=time; }
	inline float& GetTimeChain()									{ return TimeChain; }
	inline cString& GetName()										{ return name; }
	void Translate(const Vect3f &dPos);
	void Scale(const Vect3f &scale);

	inline int GetNumberAnimKey()									{ return GetNumberTri(); }
	inline sKeyTri* GetAnimKey(int number)							{ return GetTri(number); }
	inline sKeyTri* AddAnimKey()									{ return AddTri(); }
};

struct sKFrame
{
	float	*Time;
	float	count;							
	int		NumberKey;
	int		attribute;

	sKFrame(sKFrame *kf=0);
	~sKFrame()								{ if(Time) delete Time; }
	int GetNumber();
	void Set(void *time,int ofs,int nKey);
	inline float GetT(int number)			{ return (count*GetMaxTime()-Time[number-1])/(Time[number]-Time[number-1]); }
	inline float GetMaxTime()				{ if(Time) return Time[NumberKey-1]; return 0; }
	inline int IsActive()					{ return (NumberKey!=0)||(attribute!=0); }
};
struct sPKFrame : public sKFrame
{
	float *xk,*yk,*zk;						// ������������ ������������ �������������
	float *x,*y,*z;							// ���������������� �����
	sPKFrame(sPKFrame *kf=0);
	~sPKFrame()								{ if(x) delete x; if(y) delete y; if(z) delete z; if(xk) delete xk; if(yk) delete yk; if(zk) delete zk; }
	void Set(void *PosKXYZ,int nPos);
	void Scale(float sx,float sy,float sz);
	void Get(float *xPos,float *yPos,float *zPos);
};
struct sRKFrame : public sKFrame
{
	float *x,*y,*z,*w;						// ������� �����
	sRKFrame(sRKFrame *kf=0);
	~sRKFrame()								{ if(x) delete x; if(y) delete y; if(z) delete z; if(w) delete w; }
	void Set(void *RotKWXYZ,int nRot);
	void Get(float *wRot,float *xRot,float *yRot,float *zRot);
};
struct sSKFrame : public sKFrame
{
	float *sx,*sy,*sz;						// ����� ���������������
	sSKFrame(sSKFrame *kf=0);
	~sSKFrame()								{ if(sx) delete sx; if(sy) delete sy; if(sz) delete sz; }
	void Set(void *ScaleKXYZ,int nScale);
	void Get(float *xScale,float *yScale,float *zScale);
};
struct sMKFrame : public sKFrame
{
	cMesh **key;							// ����� ������������
	sMKFrame(sMKFrame *kf=0);
	~sMKFrame()								{ if(key) delete key; }
	void Set(cMesh **Morph,float *time,int nMorph);
	void Get(void *Morph);
};

class cFrame
{
public:
	cFrame(cFrame *Frame=0);
	cFrame(	char *name,char *parent,float xpivot,float ypivot,float zpivot,
			float xofs,float yofs,float zofs);
	~cFrame();
	
	int IsPhasePassed(float phase);  // ���� �������� �� ���������� ���
	void Set(float animation_period = 0, float start_phase = -1, float finish_phase = -1);
	void SetPhase(float phase);
	void AddPhase(float dt);
	inline float GetPhase()										{ return count; }
	inline float GetPeriod()									{ return dcount ? 1.f/dcount : 0; }
	inline float GetFinish()									{ return finish; }
	inline float GetStartPhase()								
	{ 
#ifdef _SURMAP_
		return StartCount; 
#endif
		return count; 
	}

	inline int IsAnimated()										{ return dcount != 0 || count != prev_count; }

	// ���������� ������������ �������, ���������� <0 � ������ ������
	int SetCurrentChain(char *NameChain); 
	inline cAnimChain*& GetAnimChain(int number)				{ assert(0<=number&&number<GetNumberAnimChain()); return AnimChain[number]; }
	inline int SetCurrentChain(int number)						{ if(number<0||number>=GetNumberAnimChain()) return -1; return ChainNumber=number; }
	inline int GetCurrentChain()								{ return ChainNumber; }

	// ���������� ��� ������ cMesh � cMeshLibrary
	void Scale(const Vect3f &scale);
	void Translate(const Vect3f &dPos);
	inline int GetPosition(Vect3f &dPos)						{ if(pkf) { pkf->Get(&dPos.x,&dPos.y,&dPos.z); return 1; } dPos.set(0,0,0); return 0; }
	inline int GetRotation(float *dwq,float *dxq,float *dyq,float *dzq){ if(rkf) { rkf->Get(dwq,dxq,dyq,dzq); return 1; } *dwq=1; *dxq=*dyq=*dzq=0; return 0; }
	inline int GetScale(Vect3f &dScale)							{ if(skf) { skf->Get(&dScale.x,&dScale.y,&dScale.z); return 1; } dScale.set(1,1,1); return 0; }
	inline int GetMorph(void *Morph)							{ if(mkf) { mkf->Get(Morph); return 1; } return 0; }
	inline void GetAnimKey(cTile *Tile)							{ if(GetNumberAnimChain()==0) return; GetAnimChain(ChainNumber)->GetTile(Tile,GetPhase()); }
	inline cAnimChain* AddAnimChain()							{ if(ChainNumber<0) ChainNumber=0; AnimChain.Resize(GetNumberAnimChain()+1); return AnimChain[GetNumberAnimChain()-1]=new cAnimChain; }
	inline int GetNumberAnimChain()								{ return AnimChain.length(); }
	int SetCurrentChain(char *NameChainMask,int number);	// ������������� ����� �� ����� ���������� ����� � ������
	int GetChain(char *NameChainMask);						// ���������� ����� ������� ������� � ����� ������ NameChainMask
private:
	float count;							// �������
	float prev_count;						// ���������� �������� ��� PassPhase
	float finish;							// �������� ����
	float dcount;							// ���������� � �������
	unsigned int looped : 1;				// ����������� 
#ifdef _SURMAP_
	float StartCount;
#endif //_SURMAP_

	inline float cycle(float t)									{ return (float) fmod(t+frame_phase_range,frame_phase_range); }
	float dist(float v0,float v1);

	sPKFrame	*pkf;
	sRKFrame	*rkf;
	sSKFrame	*skf;
	sMKFrame	*mkf;
	float xCenter,yCenter,zCenter;			
	float xPivot,yPivot,zPivot;				// ���������� ������ �������� ������������ ��������
	float xOfs,yOfs,zOfs;					// ���������� ������ �������� ������������ ���������� ������� ���������
	char Name[22];							// ��� ������
	char Parent[22];						// ��� ������-��������
	// ���������� ������������ �������
	int			ChainNumber;				// ����� ������� ������������ �������
	cBaseDynArrayPointer<cAnimChain> AnimChain; // ������������ �������

	friend class cMesh;
	friend class cMeshLibrary;
	friend class SetAnimationOp;
	inline void SetPKFrame(void *PosKXYZ,int nPos)	{ if(pkf) delete pkf; pkf=new sPKFrame; pkf->Set(PosKXYZ,nPos); }
	inline void SetRKFrame(void *RotKWXYZ,int nRot)	{ if(rkf) delete rkf; rkf=0; if(nRot>1) { rkf=new sRKFrame; rkf->Set(RotKWXYZ,nRot); } }
	inline void SetSKFrame(void *ScaleKXYZ,int nScale) { if(skf) delete skf; skf=0; if(nScale>1) { skf=new sSKFrame; skf->Set(ScaleKXYZ,nScale); } }
	inline void SetMKFrame(cMesh **Morph,float *time,int nMorph){ if(mkf) delete mkf; mkf=0; if(nMorph>1) { mkf=new sMKFrame; mkf->Set(Morph,time,nMorph); } }
};

#endif //__FRAME_H__
