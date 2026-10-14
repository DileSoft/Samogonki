//////////////////////////////////////////////////////////////////
//	Обращение к частям мехоса
//////////////////////////////////////////////////////////////////
#include "Base.h"

#include "port.h"

// Индекс для обращения к массиву деталей (0-engine, front, back, rfw, rbw, lfw, lbw)
inline int PART_INDEX(int type)	{ return BitSR(M3D_TYPE(type)); }

class Mechos;

//////////////////////////////////////////////////////////////////
//	Часть мехоса
//////////////////////////////////////////////////////////////////
class Part : public Body {
	friend class Mechos;
	friend class BodyDispatcher;

	// Link tools //////
	ShareHandle<Mechos>& targetMechos;
	// The mechos this part was detached from, for the net part sync: a client
	// rebuilding its car has to clear away the parts its own wreck left lying
	// around, and only those. 0 once the part is merged back or built fresh.
	Mechos* owner_mechos;
	cMesh* parentMesh;
	Vect3f linkOffset;
	int character; 
	float distance_to_mechos;
	int do_merge_to_mechos;
	cMesh* formic;  // Муравей
	DurationTimer colliding_timer;
	AverageConditionTimer stopping_detection_timer;


public:
	
		Part(cMesh* geometry_, int formic_);
		~Part();

	void release_body_handles();
	
	void quant();
	void calc_forces_and_drags(); 
	int is_colliding_with(const Body& body);
	void post_quant();

	void preLink(class Mechos* mechos, cMesh* mesh);
	void preLink(Mechos* mechos, const Vect3f& target);
	void merge_to_mechos();
};
