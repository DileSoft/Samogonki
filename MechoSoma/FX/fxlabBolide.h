#ifndef __FXLAB_BOLIDE_H__
#define __FXLAB_BOLIDE_H__

/*
	The bolide classes, on their own.

	Split out of fxlabBodyInterface.h because the two halves of that header
	want different things. The bolide classes are small, self contained and
	complete given Meteorite.h; the factory templates that build them take an
	fxlabProtectionType and an ArcaneStatisticsInfo and want the whole
	server-side header chain laid down first, which is a lot to drag in for a
	type that is only ever used as a parameter.

	The network layer is what needs the split. It wants the classes - to tell
	a cannon bomb from a fire bolide, and to rebuild one the host described -
	and has no use whatsoever for the template that creates them.
*/

#include "Meteorite.h"

//Bomb
class fxlabCannonBomb : public Meteorite
{
public:
	fxlabCannonBomb(int m3d_model, const Vect3f& R_, const Vect3f& V_, double damage__, int owner_ID = 0)
		: Meteorite( m3d_model, R_, V_, damage__, owner_ID) {}
	void kill() override;
};

//VolcanicBomb
class fxlabVolcanicBomb : public Meteorite
{
public:
	fxlabVolcanicBomb(int m3d_model, const Vect3f& R_, const Vect3f& V_, double damage__, int owner_ID = 0)
		: Meteorite( m3d_model, R_, V_, damage__, owner_ID) {}
	void kill() override;
};

//FireBolide
class fxlabFireBolide : public StatisticalMeteorite
{
public:
	fxlabFireBolide(int m3d_model, const Vect3f& R_, const Vect3f& V_, double damage__, int owner_ID = 0)
		: StatisticalMeteorite( m3d_model, R_, V_, damage__, owner_ID) { scaleRadius(2); }
	void kill() override;
};

//FireBolide
class fxlabSnowBall : public StatisticalMeteorite
{
public:
	fxlabSnowBall(int m3d_model, const Vect3f& R_, const Vect3f& V_, double damage__, int owner_ID = 0);
	void body_overlap_reaction(Body& body) override;
	void calc_forces_and_drags() override;
	void kill() override;
};

//Mortar
class fxlabSleepyMortar : public BonusMeteorite
{
	MeasurementTimer color_time;
public:
	fxlabSleepyMortar(int m3d_model, const Vect3f& R_, const Vect3f& V_, double damage__, int owner_ID = 0);
	void kill() override;
	void quant() override;
};

//MinorBonus
class fxlabMinorBonus : public BonusMeteorite
{
public:
	fxlabMinorBonus(int m3d_model, const Vect3f& R_, const Vect3f& V_, float damage__, int owner_ID)
		: BonusMeteorite( m3d_model, R_, V_, damage__, owner_ID) {}
	void body_overlap_reaction(Body& body) override;
	void kill() override;
};

//MajorBonus
class fxlabMajorBonus : public BonusMeteorite
{
public:
	fxlabMajorBonus(int m3d_model, const Vect3f& R_, const Vect3f& V_, double damage__, int owner_ID)
		: BonusMeteorite( m3d_model, R_, V_, damage__, owner_ID){ type_ = REAL_BONUS_METEORITE; }
	void body_overlap_reaction(Body& body) override;
	void kill() override;
};

#endif // __FXLAB_BOLIDE_H__
