#ifndef __FXLAB_BODY_INTERFACE_H__
#define __FXLAB_BODY_INTERFACE_H__

/*
	The bolide factory.

	The bolide classes themselves live in fxlabBolide.h so that a layer which
	only needs to name and rebuild them does not have to instantiate this
	header's templates, or lay down the server-side chain those templates want.
*/

#include "fxlabBolide.h"

#include "Meteorite.h"

template<class ObjectType>
void fxlabCreateBolide(int model,int noise,const Vect3f& pos,const Vect3f& vel,float damage,int client_key_type,ArcaneStatisticsInfo* stat,fxlabProtectionType* protect,ObjectType* stuff)
{
	Body* p;
	OwnerProtection* op;
	fxlabGeneralObjectType* ct;

	if(stat) ArcaneStatisticsService::setCurrentArcane(*stat);
	p = new ObjectType(M3D_TOTAL_TYPE(model + fxlabServerRND.RND(noise),M3D_XREAL), pos,vel,damage,-1);
	if(protect){
		op = dynamic_cast<OwnerProtection*>(p);
		if(op) op->set_owner(protect->ProtectionID,protect->ProtectionTime);
	};
	Mdisp -> attachBody(p);

	if(client_key_type >= 0){
		ct = fxlabClientD->CreateObject(client_key_type);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetPosition(pos);
		ct->SetVelocity(vel);
		ct->SetBody(p);
		ct->Start();
	};
};

template<class ObjectType>
void fxlabCreateSplinter(int model,int noise,Vect3f& pos,float rate,float start_psi,float thetta,float delta_thetta,float speed,float damage,int client_key_type,class ArcaneStatisticsInfo* stat,struct fxlabProtectionType* protect,ObjectType* stuff)
{
	int i,n;
	float psi,dpsi,f;
	Vect3f v;

	f = floor(rate);
	if(fxlabServerRND.UnitRND() < rate - f)
		n = round(f) + 1;
	else
		n = round(f);

	psi = start_psi;
	dpsi = 2.0f * M_PI / n;

	for(i = 0;i < n;i++){
		v.setSpherical(psi,thetta + fxlabServerRND.UnitRND() * delta_thetta,speed);
		fxlabCreateBolide<ObjectType>(model,noise,pos,v,damage,client_key_type,stat,protect,NULL);

		psi += dpsi;
	};
};

#endif // __FXLAB_BODY_INTERFACE_H__
