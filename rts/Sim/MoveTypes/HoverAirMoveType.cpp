/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/mmgr.h"

#include "HoverAirMoveType.h"
#include "Game/Player.h"
#include "Map/Ground.h"
#include "Sim/Misc/GeometricObjects.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Projectiles/Unsynced/SmokeProjectile.h"
#include "Sim/Units/Scripts/UnitScript.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitTypes/TransportUnit.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "System/myMath.h"
#include "System/Matrix44f.h"


CR_BIND_DERIVED(CHoverAirMoveType, AAirMoveType, (NULL));

CR_REG_METADATA(CHoverAirMoveType, (
	CR_MEMBER(loadingUnits),
	CR_MEMBER(bankingAllowed),
	CR_MEMBER(airStrafe),

	CR_MEMBER(circlingPos),
	CR_MEMBER(goalDistance),
	CR_MEMBER(waitCounter),
	CR_MEMBER(wantToStop),

	CR_MEMBER(wantedHeading),

	CR_MEMBER(wantedSpeed),
	CR_MEMBER(deltaSpeed),

	CR_MEMBER(currentBank),
	CR_MEMBER(currentPitch),

	CR_MEMBER(turnRate),
	CR_MEMBER(accRate),
	CR_MEMBER(decRate),
	CR_MEMBER(altitudeRate),

	CR_MEMBER(brakeDistance),
	CR_MEMBER(dontLand),
	CR_MEMBER(lastMoveRate),

	CR_MEMBER(forceHeading),
	CR_MEMBER(forceHeadingTo),

	CR_MEMBER(maxDrift),

	CR_MEMBER(randomWind),

	CR_RESERVED(32)
));


CHoverAirMoveType::CHoverAirMoveType(CUnit* owner) :
	AAirMoveType(owner),
	flyState(FLY_CRUISING),
	loadingUnits(false),
	bankingAllowed(true),
	airStrafe(owner->unitDef->airStrafe),
	circlingPos(ZeroVector),
	goalDistance(1),
	waitCounter(0),
	wantToStop(false),
	// we want to take off in direction of factory facing
	wantedHeading(GetHeadingFromFacing(owner->buildFacing)),
	wantedSpeed(ZeroVector),
	deltaSpeed(ZeroVector),
	currentBank(0),
	currentPitch(0),
	turnRate(1),
	accRate(1),
	decRate(1),
	altitudeRate(3.0f),
	brakeDistance(1),
	dontLand(false),
	lastMoveRate(0),
	forceHeading(false),
	forceHeadingTo(wantedHeading),
	maxDrift(1)
{
	assert(owner != NULL);
	assert(owner->unitDef != NULL);

	turnRate = owner->unitDef->turnRate;
	accRate = std::max(0.01f, owner->unitDef->maxAcc);
	decRate = std::max(0.01f, owner->unitDef->maxDec);

	wantedHeight = owner->unitDef->wantedHeight + gs->randFloat(owner) * 5.0f;
	orgWantedHeight = wantedHeight;
	dontLand = owner->unitDef->DontLand();
	collide = owner->unitDef->collide;
	altitudeRate = owner->unitDef->verticalSpeed;
	bankingAllowed = owner->unitDef->bankingAllowed;
	useSmoothMesh = owner->unitDef->useSmoothMesh;

	// prevent weapons from being updated and firing while on the ground
	owner->dontUseWeapons = true;
}


void CHoverAirMoveType::SetGoal(float3 newPos, float distance)
{
	// aircraft need some marginals to avoid uber stacking
	// when lots of them are ordered to one place
	maxDrift = std::max(16.0f, distance);
	goalPos = newPos;
	oldGoalPos = newPos;
	forceHeading = false;
	wantedHeight = orgWantedHeight;
}

void CHoverAirMoveType::SetState(AircraftState newState)
{
	// once in crashing, we should never change back into another state
	assert(aircraftState != AIRCRAFT_CRASHING || newState == AIRCRAFT_CRASHING);

	if (newState == aircraftState) {
		return;
	}

	// Perform cob animation
	if (aircraftState == AIRCRAFT_LANDED) {
		assert(newState != AIRCRAFT_LANDING); // redundant SetState() call, we already landed and get command to switch into landing
		owner->QueScriptStartMoving();
	}

	if (newState == AIRCRAFT_LANDED) {
		owner->dontUseWeapons = true;
		owner->useAirLos = false;
	} else {
		owner->dontUseWeapons = false;
		owner->useAirLos = true;
	}

	aircraftState = newState;

	switch (aircraftState) {
		case AIRCRAFT_CRASHING:
			owner->SetCrashing(true);
			break;
		case AIRCRAFT_LANDED:
			//FIXME already inform commandAI in AIRCRAFT_LANDING!
			//FIXME Problem is StopMove() also calls owner->script->StopMoving() what should only be called when landed. Also see CStrafeAirMoveType::SetState().
			owner->QueCAIStopMove();
			if (padStatus == PAD_STATUS_FLYING) {
				// set us on ground if we are not on a pad
				owner->physicalState = CSolidObject::OnGround;
				owner->QueBlock();
			}
			break;
		case AIRCRAFT_LANDING:
			owner->QueDeactivate();
			break;
		case AIRCRAFT_HOVERING:
			wantedHeight = orgWantedHeight;
			wantedSpeed = ZeroVector;
			// fall through
		default:
			owner->physicalState = CSolidObject::Flying;
			owner->QueUnBlock();
			owner->QueActivate();
			reservedLandingPos.x = -1;
			break;
	}

	// Cruise as default
	if (aircraftState == AIRCRAFT_FLYING || aircraftState == AIRCRAFT_HOVERING) {
		flyState = FLY_CRUISING;
	}

	owner->isMoving = (aircraftState != AIRCRAFT_LANDED);
	waitCounter = 0;
}

void CHoverAirMoveType::StartMoving(float3 pos, float goalRadius)
{
	wantToStop = false;
	owner->isMoving = true;
	waitCounter = 0;
	forceHeading = false;

	switch (aircraftState) {
		case AIRCRAFT_LANDED:
			SetState(AIRCRAFT_TAKEOFF);
			break;
		case AIRCRAFT_TAKEOFF:
			SetState(AIRCRAFT_TAKEOFF);
			break;
		case AIRCRAFT_FLYING:
			if (flyState != FLY_CRUISING)
				flyState = FLY_CRUISING;
			break;
		case AIRCRAFT_LANDING:
			SetState(AIRCRAFT_TAKEOFF);
			break;
		case AIRCRAFT_HOVERING:
			SetState(AIRCRAFT_FLYING);
			break;
		case AIRCRAFT_CRASHING:
			break;
	}

	SetGoal(pos, goalRadius);
	brakeDistance = ((maxSpeed * maxSpeed) / decRate);
	progressState = AMoveType::Active;
}

void CHoverAirMoveType::StartMoving(float3 pos, float goalRadius, float speed)
{
	StartMoving(pos, goalRadius);
}

void CHoverAirMoveType::KeepPointingTo(float3 pos, float distance, bool aggressive)
{
	wantToStop = false;
	forceHeading = false;
	wantedHeight = orgWantedHeight;
	// close in a little to avoid the command AI to override the pos constantly
	distance -= 15;

	// Ignore the exact same order
	if ((aircraftState == AIRCRAFT_FLYING)
			&& (flyState == FLY_CIRCLING || flyState == FLY_ATTACKING)
			&& ((circlingPos - pos).SqLength2D() < 64)
			&& (goalDistance == distance))
	{
		return;
	}

	circlingPos = pos;
	goalDistance = distance;
	goalPos = owner->pos;

	SetState(AIRCRAFT_FLYING);

	if (aggressive) {
		flyState = FLY_ATTACKING;
	} else {
		flyState = FLY_CIRCLING;
	}
}

void CHoverAirMoveType::ExecuteStop()
{
	wantToStop = false;
	wantedSpeed = ZeroVector;

	switch (aircraftState) {
		case AIRCRAFT_TAKEOFF: {
			if (!dontLand && autoLand) {
				SetState(AIRCRAFT_LANDING);
				// trick to land directly
				waitCounter = GAME_SPEED;
				break;
			}
		} // fall through
		case AIRCRAFT_FLYING: {
			goalPos = owner->pos;

			if (owner->unitDef->DontLand() || dontLand || !autoLand) {
				SetState(AIRCRAFT_HOVERING);
			} else {
				SetState(AIRCRAFT_LANDING);
			}
		} break;
		case AIRCRAFT_LANDING: {} break;
		case AIRCRAFT_LANDED: {} break;
		case AIRCRAFT_CRASHING: {} break;
		case AIRCRAFT_HOVERING: {
			if (!dontLand && autoLand) {
				// land immediately
				SetState(AIRCRAFT_LANDING);
				waitCounter = GAME_SPEED;
			}
		} break;
	}
}

void CHoverAirMoveType::StopMoving()
{
	wantToStop = true;
	forceHeading = false;
	owner->isMoving = false;
	wantedHeight = orgWantedHeight;

	if (progressState != AMoveType::Failed)
		progressState = AMoveType::Done;
}



void CHoverAirMoveType::UpdateLanded()
{
	AAirMoveType::UpdateLanded();

	if (progressState != AMoveType::Failed)
		progressState = AMoveType::Done;
}

void CHoverAirMoveType::UpdateTakeoff()
{
	const float3& pos = owner->pos;

	wantedSpeed = ZeroVector;
	wantedHeight = orgWantedHeight;

	UpdateAirPhysics();

	const float altitude = owner->unitDef->canSubmerge?
		(pos.y - ground->GetHeightReal(pos.x, pos.z)):
		(pos.y - ground->GetHeightAboveWater(pos.x, pos.z));

	if (altitude > orgWantedHeight * 0.8f) {
		SetState(AIRCRAFT_FLYING);
	}
}


// Move the unit around a bit..
void CHoverAirMoveType::UpdateHovering()
{
	#define NOZERO(x) std::max(x, 0.0001f)

	const float driftSpeed = math::fabs(owner->unitDef->dlHoverFactor);
	float3 deltaVec = goalPos - owner->pos;
	float3 deltaDir = float3(deltaVec.x, 0.0f, deltaVec.z);
	float l = NOZERO(deltaDir.Length2D());
	deltaDir *= smoothstep(0.0f, 20.0f, l) / l;

	// move towards goal position if it's not immediately
	// behind us when we have more waypoints to get to
	// *** this behavior interferes with the loading procedure of transports ***
	if (aircraftState != AIRCRAFT_LANDING && owner->commandAI->HasMoreMoveCommands() &&
		(l < 120) && (deltaDir.SqDistance(deltaVec) > 1.0f) && dynamic_cast<CTransportUnit*>(owner) == NULL) {
		deltaDir = owner->frontdir;
	}

	deltaDir -= owner->speed;
	l = deltaDir.SqLength2D();
	if (l > (maxSpeed * maxSpeed)) {
		deltaDir *= maxSpeed / NOZERO(math::sqrt(l));
	}

	// random movement (a sort of fake wind effect)
	// random drift values are in range -0.5 ... 0.5
	randomWind.x = randomWind.x * 0.9f + (gs->randFloat(owner) - 0.5f) * 0.5f;
	randomWind.z = randomWind.z * 0.9f + (gs->randFloat(owner) - 0.5f) * 0.5f;

	wantedSpeed = owner->speed + deltaDir;
	wantedSpeed += (randomWind * driftSpeed * 0.5f);

	UpdateAirPhysics();
}


void CHoverAirMoveType::UpdateFlying()
{
	const float3& pos = owner->pos;
	const float3& speed = owner->speed;

	// Direction to where we would like to be
	float3 goalVec = goalPos - pos;

	owner->restTime = 0;

	// don't change direction for waypoints we just flew over and missed slightly
	if (flyState != FLY_LANDING && owner->commandAI->HasMoreMoveCommands()) {
		float3 goalDir = goalVec;

		if ((goalDir != ZeroVector) && (goalDir.UnsafeANormalize()).SqDistance(goalVec) < 1.0f) {
			goalVec = owner->frontdir;
		}
	}

	const float goalDistSq2D = goalVec.SqLength2D();
	const float gHeight = UseSmoothMesh()?
		std::max(smoothGround->GetHeight(pos.x, pos.z), ground->GetApproximateHeight(pos.x, pos.z)):
		ground->GetHeightAboveWater(pos.x, pos.z);

	const bool closeToGoal = (flyState == FLY_ATTACKING)?
		(goalDistSq2D < (             400.0f)):
		(goalDistSq2D < (maxDrift * maxDrift)) && (math::fabs(gHeight - pos.y + wantedHeight) < maxDrift);

	if (closeToGoal) {
		switch (flyState) {
			case FLY_CRUISING: {
				const bool hasMoreMoveCmds = owner->commandAI->HasMoreMoveCommands();
				const bool noland = dontLand || !autoLand || hasMoreMoveCmds;

				// NOTE: should CMD_LOAD_ONTO be here?
				const bool trans = (dynamic_cast<CTransportUnit*>(owner) != NULL);
				const bool hasLoadCmds = trans &&
					!owner->commandAI->commandQue.empty() &&
					(owner->commandAI->commandQue.front().GetID() == CMD_LOAD_ONTO ||
					 owner->commandAI->commandQue.front().GetID() == CMD_LOAD_UNITS);
				// [?] transport aircraft need some time to detect that they can pickup
				const bool canLoad = trans && (++waitCounter < ((GAME_SPEED << 1) - 5));

				if (noland || (canLoad && hasLoadCmds)) {
					if (trans) {
						wantedSpeed = ZeroVector;
						SetState(AIRCRAFT_HOVERING);
						if (waitCounter > (GAME_SPEED << 1)) {
							wantedHeight = orgWantedHeight;
						}
					} else {
						wantedSpeed = ZeroVector;
						if (!hasMoreMoveCmds) {
							wantToStop = true;
							SetState(AIRCRAFT_HOVERING);
						}
					}
				} else {
					wantedHeight = orgWantedHeight;
					SetState(AIRCRAFT_LANDING);
				}
				return;
			}
			case FLY_CIRCLING:
				// break;
				if ((++waitCounter) > ((GAME_SPEED * 3) + 10)) {
					if (airStrafe) {
						float3 relPos = pos - circlingPos;
						if (relPos.x < 0.0001f && relPos.x > -0.0001f) {
							relPos.x = 0.0001f;
						}
						relPos.y = 0.0f;
						relPos.Normalize();
						static CMatrix44f rot(0.0f,fastmath::PI/4.0f,0.0f);
						float3 newPos = rot.Mul(relPos);

						// Make sure the point is on the circle
						newPos = newPos * goalDistance;

						// Go there in a straight line
						goalPos = circlingPos + newPos;
					}
					waitCounter = 0;
				}
				break;
			case FLY_ATTACKING: {
				if (airStrafe) {
					float3 relPos = pos - circlingPos;
					if (relPos.x < 0.0001f && relPos.x > -0.0001f) {
						relPos.x = 0.0001f;
					}
					relPos.y = 0;
					relPos.Normalize();
					CMatrix44f rot;
					if (gs->randFloat(owner) > 0.5f) {
						rot.RotateY(0.6f + gs->randFloat(owner) * 0.6f);
					} else {
						rot.RotateY(-(0.6f + gs->randFloat(owner) * 0.6f));
					}
					float3 newPos = rot.Mul(relPos);
					newPos = newPos * goalDistance;

					// Go there in a straight line
					goalPos = circlingPos + newPos;
				}
				break;
			}
			case FLY_LANDING: {
				break;
			}
		}
	}

	// not there yet, so keep going
	goalVec.y = 0.0f;

	// if we are close to our goal and not in attack mode,
	// we should go slow enough to be able to break in time
	const float goalDist = goalVec.Length() + 0.1f;
	const float approachSpeed =
		(flyState != FLY_ATTACKING && goalDist < brakeDistance)?
		(goalDist / (speed.Length2D() + 0.01f) * decRate):
		maxSpeed;

	wantedSpeed = (goalVec / goalDist) * approachSpeed;

	UpdateAirPhysics();

	// Point toward goal or forward - unless we just passed it to get to another goal
	if ((flyState == FLY_ATTACKING) || (flyState == FLY_CIRCLING)) {
		goalVec = circlingPos - pos;
	} else {
		const bool b0 = (flyState != FLY_LANDING && (owner->commandAI->HasMoreMoveCommands()));
		const bool b1 = (goalDist < 120.0f) && ((goalVec.SafeNormalize()).SqDistance(goalVec) > 1.0f);

		if (b0 && b1) {
			goalVec = owner->frontdir;
		} else {
			goalVec = goalPos - pos;
		}
	}

	if (goalVec.SqLength2D() > 1.0f) {
		wantedHeading = GetHeadingFromVector(goalVec.x, goalVec.z);
	}
}



void CHoverAirMoveType::UpdateLanding()
{
	const float3& pos = owner->pos;
	const float3& speed = owner->speed;

	// We want to land, and therefore cancel our speed first
	wantedSpeed = ZeroVector;

	// Hang around for a while so queued commands have a chance to take effect
	if ((++waitCounter) < GAME_SPEED) {
		UpdateAirPhysics();
		return;
	}

	if (reservedLandingPos.x < 0.0f) {
		if (CanLandAt(pos)) {
			// found a landing spot
			reservedLandingPos = pos;
			goalPos = pos;
			owner->physicalState = CSolidObject::OnGround;
			owner->QueBlock();
			owner->physicalState = CSolidObject::Flying;
			owner->QueDeactivate();
			owner->QueScriptStopMoving();
		} else {
			if (goalPos.SqDistance2D(pos) < 900) {
				goalPos = goalPos + gs->randVector(owner) * 300;
				goalPos.ClampInBounds();
				progressState = AMoveType::Failed; // exact landing pos failed, make sure finishcommand is called anyway
			}
			flyState = FLY_LANDING;
			UpdateFlying();
			return;
		}
	}
	// We should wait until we actually have stopped smoothly
	if (speed.SqLength2D() > 1.0f) {
		UpdateFlying();
		UpdateAirPhysics();
		return;
	}

	// We have stopped, time to land
	// NOTE: wantedHeight is interpreted as RELATIVE altitude
	const float gh = ground->GetHeightAboveWater(pos.x, pos.z);
	const float gah = ground->GetHeightReal(pos.x, pos.z);
	float altitude = (wantedHeight = 0.0f);

	// can we submerge and are we still above water?
	if ((owner->unitDef->canSubmerge) && (gah < 0.0f)) {
		altitude = pos.y - gah;
	} else {
		altitude = pos.y - gh;
	}

	UpdateAirPhysics();

	// collision detection does not let us get
	// closer to the ground than <radius> elmos
	// (wrt. midPos.y)
	if (altitude <= owner->radius) {
		SetState(AIRCRAFT_LANDED);
	}
}



void CHoverAirMoveType::UpdateHeading()
{
	if (aircraftState == AIRCRAFT_TAKEOFF && !owner->unitDef->factoryHeadingTakeoff)
		return;

	SyncedSshort& heading = owner->heading;
	const short deltaHeading = forceHeading?
		(forceHeadingTo - heading):
		(wantedHeading - heading);

	if (deltaHeading > 0) {
		heading += std::min(deltaHeading, short(turnRate));
	} else {
		heading += std::max(deltaHeading, short(-turnRate));
	}

	owner->UpdateDirVectors(aircraftState == AIRCRAFT_LANDED);
	owner->UpdateMidAndAimPos();
}

void CHoverAirMoveType::UpdateBanking(bool noBanking)
{
	if (aircraftState != AIRCRAFT_FLYING && aircraftState != AIRCRAFT_HOVERING)
		return;

	if (!owner->upright) {
		float wantedPitch = 0.0f;

		if (aircraftState == AIRCRAFT_FLYING && flyState == FLY_ATTACKING && circlingPos.y < owner->pos.y) {
			wantedPitch = (circlingPos.y - owner->pos.y) / circlingPos.distance(owner->pos);
		}

		currentPitch = currentPitch * 0.95f + wantedPitch * 0.05f;
	}

	const float bankLimit = std::min(1.0f, goalPos.SqDistance2D(owner->pos) * Square(0.15f));
	float wantedBank = 0.0f;

	SyncedFloat3& frontDir = owner->frontdir;
	SyncedFloat3& upDir = owner->updir;
	SyncedFloat3& rightDir3D = owner->rightdir;
	SyncedFloat3  rightDir2D;

	// pitching does not affect rightdir, but...
	frontDir.y = currentPitch;
	frontDir.Normalize();

	// we want a flat right-vector to calculate wantedBank
	rightDir2D = frontDir.cross(UpVector);

	if (!noBanking && bankingAllowed)
		wantedBank = rightDir2D.dot(deltaSpeed) / accRate * 0.5f;

	if (Square(wantedBank) > bankLimit) {
		wantedBank =  math::sqrt(bankLimit);
	} else if (Square(wantedBank) < -bankLimit) {
		wantedBank = -math::sqrt(bankLimit);
	}

	// Adjust our banking to the desired value
	if (currentBank > wantedBank) {
		currentBank -= std::min(0.03f, currentBank - wantedBank);
	} else {
		currentBank += std::min(0.03f, wantedBank - currentBank);
	}


	upDir = rightDir2D.cross(frontDir);
	upDir = upDir * math::cos(currentBank) + rightDir2D * math::sin(currentBank);
	rightDir3D = frontDir.cross(upDir);

	// NOTE:
	//     heading might not be fully in sync with frontDir due to the
	//     vector<-->heading mapping not being 1:1 (such that heading
	//     != GetHeadingFromVector(frontDir)), therefore this call can
	//     cause owner->heading to change --> unwanted if forceHeading
	//
	//     it is "safe" to skip because only frontDir.y is manipulated
	//     above so its xz-direction does not change, but the problem
	//     should really be fixed elsewhere
	if (!forceHeading) {
		owner->SetHeadingFromDirection();
	}

	owner->UpdateMidAndAimPos();
}


void CHoverAirMoveType::UpdateAirPhysics()
{
	const float3& pos = owner->pos;
	      float3& speed = owner->speed;

	if (!((gs->frameNum + owner->id) & 3)) {
		CheckForCollision();
	}

	const float yspeed = speed.y; speed.y = 0.0f;

	const float3 deltaSpeed = wantedSpeed - speed;
	const float deltaDotSpeed = (speed != ZeroVector)? deltaSpeed.dot(speed): 1.0f;

	if (deltaDotSpeed == 0.0f) {
		// we have the wanted speed
	} else if (deltaDotSpeed > 0.0f) {
		// accelerate
		const float sqdl = deltaSpeed.SqLength();

		if (sqdl < Square(accRate)) {
			speed = wantedSpeed;
		} else {
			speed += (deltaSpeed / math::sqrt(sqdl) * accRate);
		}
	} else {
		// break
		const float sqdl = deltaSpeed.SqLength();

		if (sqdl < Square(decRate)) {
			speed = wantedSpeed;
		} else {
			speed += (deltaSpeed / math::sqrt(sqdl) * decRate);
		}
	}

	// absolute and relative ground height at (pos.x, pos.z)
	// if this aircraft uses the smoothmes, these values are
	// calculated with respect to that for changing vertical
	// speed, but not for ground collision
	float curAbsHeight = owner->unitDef->canSubmerge?
		ground->GetHeightReal(pos.x, pos.z):
		ground->GetHeightAboveWater(pos.x, pos.z);
	float curRelHeight = 0.0f;

	float wh = wantedHeight; // wanted RELATIVE height (altitude)
	float ws = 0.0f;         // wanted vertical speed

	// always stay above the actual terrain (therefore either the value of
	// <midPos.y - radius> or pos.y must never become smaller than the real
	// ground height)
	// note: unlike StrafeAirMoveType, UpdateTakeoff calls UpdateAirPhysics
	// so we ignore terrain while we are in the takeoff state to avoid jumps
	if (modInfo.allowAircraftToHitGround) {
		const bool groundContact = (curAbsHeight > (owner->midPos.y - owner->radius));
		const bool handleContact = (aircraftState != AIRCRAFT_LANDED && aircraftState != AIRCRAFT_TAKEOFF);

		if (groundContact && handleContact) {
			owner->Move1D(curAbsHeight - (owner->midPos.y - owner->radius) + 0.01f, 1, true);
		}
	}

	if (UseSmoothMesh()) {
		curAbsHeight = owner->unitDef->canSubmerge?
			smoothGround->GetHeight(pos.x, pos.z):
			smoothGround->GetHeightAboveWater(pos.x, pos.z);
	}

	speed.y = yspeed;
	curRelHeight = pos.y - curAbsHeight;

	if (lastColWarningType == 2) {
		const float3 dir = lastColWarning->StableMidPos() - owner->midPos;
		const float3 sdir = lastColWarning->StableSpeed() - speed;

		if (speed.dot(dir + sdir * 20.0f) < 0.0f) {
			if (lastColWarning->StableMidPos().y > owner->pos.y) {
				wh -= 30.0f;
			} else {
				wh += 50.0f;
			}
		}
	}



	if (curRelHeight < wh) {
		ws = altitudeRate;

		if ((speed.y > 0.0001f) && (((wh - curRelHeight) / speed.y) * accRate * 1.5f) < speed.y) {
			ws = 0.0f;
		}
	} else {
		ws = -altitudeRate;

		if ((speed.y < -0.0001f) && (((wh - curRelHeight) / speed.y) * accRate * 0.7f) < -speed.y) {
			ws = 0.0f;
		}
	}

	if (!owner->beingBuilt) {
		if (math::fabs(wh - curRelHeight) > 2.0f) {
			if (speed.y > ws) {
				speed.y = std::max(ws, speed.y - accRate * 1.5f);
			} else {
				// accelerate upward faster if close to ground
				speed.y = std::min(ws, speed.y + accRate * ((curRelHeight < 20.0f)? 2.0f: 0.7f));
			}
		} else {
			speed.y *= 0.95;
		}
	}


	if (modInfo.allowAircraftToLeaveMap || (pos + speed).IsInBounds()) {
		owner->Move3D(speed, true);
	}
}


void CHoverAirMoveType::UpdateMoveRate()
{
	int curRate = 0;

	// currentspeed is not used correctly for vertical movement, so compensate with this hax
	if ((aircraftState == AIRCRAFT_LANDING) || (aircraftState == AIRCRAFT_TAKEOFF)) {
		curRate = 1;
	} else {
		const float curSpeed = owner->speed.Length();

		curRate = (curSpeed / maxSpeed) * 3;
		curRate = std::max(0, std::min(curRate, 2));
	}

	if (curRate != lastMoveRate) {
		owner->QueScriptMoveRate(curRate);
		lastMoveRate = curRate;
	}
}


bool CHoverAirMoveType::Update()
{
	const float3& pos = owner->pos;
	      float3& speed = owner->speed;

	AAirMoveType::Update();

	if ((owner->IsStunned() && !owner->IsCrashing()) || owner->beingBuilt) {
		wantedSpeed = ZeroVector;

		UpdateAirPhysics();
		return (HandleCollisions());
	}

	// allow us to stop if wanted (changes aircraft state)
	if (wantToStop)
		ExecuteStop();

	const float3 lastSpeed = speed;

	if (aircraftState != AIRCRAFT_CRASHING) {
		if (owner->fpsControlPlayer != NULL) {
			SetState(AIRCRAFT_FLYING);

			const FPSUnitController& con = owner->fpsControlPlayer->fpsController;

			const float3 forward = con.viewDir;
			const float3 right = forward.cross(UpVector);
			const float3 nextPos = pos + speed;

			float3 flatForward = forward;
			flatForward.y = 0.0f;
			flatForward.Normalize();

			wantedSpeed = ZeroVector;

			if (con.forward) wantedSpeed += flatForward;
			if (con.back   ) wantedSpeed -= flatForward;
			if (con.right  ) wantedSpeed += right;
			if (con.left   ) wantedSpeed -= right;

			wantedSpeed.Normalize();
			wantedSpeed *= maxSpeed;

			if (!nextPos.IsInBounds()) {
				speed = ZeroVector;
			}

			UpdateAirPhysics();
			wantedHeading = GetHeadingFromVector(flatForward.x, flatForward.z);
		}

		if (reservedPad != NULL) {
			MoveToRepairPad();

			if (padStatus >= PAD_STATUS_LANDING) {
				flyState = FLY_LANDING;
			}
		}
	}

	switch (aircraftState) {
		case AIRCRAFT_LANDED:
			UpdateLanded();
			break;
		case AIRCRAFT_TAKEOFF:
			UpdateTakeoff();
			break;
		case AIRCRAFT_FLYING:
			UpdateFlying();
			break;
		case AIRCRAFT_LANDING:
			UpdateLanding();
			break;
		case AIRCRAFT_HOVERING:
			UpdateHovering();
			break;
		case AIRCRAFT_CRASHING: {
			UpdateAirPhysics();

			if ((ground->GetHeightAboveWater(owner->pos.x, owner->pos.z) + 5.0f + owner->radius) > owner->pos.y) {
				owner->SetCrashing(false);
				owner->KillUnit(true, false, 0);
			} else {
				#define SPIN_DIR(o) ((o->id & 1) * 2 - 1)
				wantedHeading = GetHeadingFromVector(owner->rightdir.x * SPIN_DIR(owner), owner->rightdir.z * SPIN_DIR(owner));
				wantedHeight = 0.0f;
				#undef SPIN_DIR
			}

			new CSmokeProjectile(owner->midPos, gs->randVector() * 0.08f, 100 + gs->randFloat() * 50, 5, 0.2f, owner, 0.4f);
		} break;
	}


	// Banking requires deltaSpeed.y = 0
	deltaSpeed = speed - lastSpeed;
	deltaSpeed.y = 0.0f;

	// Turn and bank and move; update dirs
	UpdateHeading();
	UpdateBanking(aircraftState == AIRCRAFT_HOVERING);

	return (HandleCollisions());
}

void CHoverAirMoveType::SlowUpdate()
{
	ASSERT_SINGLETHREADED_SIM();
	UpdateFuel();

	// HoverAirMoveType aircraft are controlled by AirCAI's,
	// but only MobileCAI's reserve pads so we need to do
	// this for ourselves
	if (reservedPad == NULL && aircraftState == AIRCRAFT_FLYING && WantsRepair()) {
		CAirBaseHandler::LandingPad* lp = airBaseHandler->FindAirBase(owner, owner->unitDef->minAirBasePower, true);

		if (lp != NULL) {
			AAirMoveType::ReservePad(lp);
		}
	}

	UpdateMoveRate();
	// note: NOT AAirMoveType::SlowUpdate
	AMoveType::SlowUpdate();
}

/// Returns true if indicated position is a suitable landing spot
bool CHoverAirMoveType::CanLandAt(const float3& pos) const
{
	if (dontLand) { return false; }
	if (!autoLand) { return false; }
	if (!pos.IsInBounds()) { return false; }

	const UnitDef* ud = owner->unitDef;
	const float gah = ground->GetApproximateHeight(pos.x, pos.z);

	if ((gah < 0.0f) && !(ud->floatOnWater || ud->canSubmerge)) {
		return false;
	}

	const int2 mp = owner->GetMapPos(pos);

	for (int z = mp.y; z < mp.y + owner->zsize; z++) {
		for (int x = mp.x; x < mp.x + owner->xsize; x++) {
			if (groundBlockingObjectMap->GroundBlocked(x, z, owner)) {
				return false;
			}
		}
	}

	return true;
}

void CHoverAirMoveType::ForceHeading(short h)
{
	forceHeading = true;
	forceHeadingTo = h;
}

void CHoverAirMoveType::SetWantedAltitude(float altitude)
{
	if (altitude == 0) {
		wantedHeight = orgWantedHeight;
	} else {
		wantedHeight = altitude;
	}
}

void CHoverAirMoveType::SetDefaultAltitude(float altitude)
{
	wantedHeight =  altitude;
	orgWantedHeight = altitude;
}

void CHoverAirMoveType::Takeoff()
{
	if (aircraftState == AAirMoveType::AIRCRAFT_LANDED) {
		SetState(AAirMoveType::AIRCRAFT_TAKEOFF);
	}
	if (aircraftState == AAirMoveType::AIRCRAFT_LANDING) {
		SetState(AAirMoveType::AIRCRAFT_FLYING);
	}
}

void CHoverAirMoveType::Land()
{
	if (aircraftState == AAirMoveType::AIRCRAFT_HOVERING) {
		SetState(AAirMoveType::AIRCRAFT_FLYING); // switch to flying, it performs necessary checks to prepare for landing
	}
}

bool CHoverAirMoveType::HandleCollisions()
{
	const float3& pos = owner->pos;

	if (pos != oldPos) {
		oldPos = pos;

		// check for collisions if not on a pad, not being built, or not taking off
		// includes an extra condition for transports, which are exempt while loading
		const bool checkCollisions = collide && !owner->beingBuilt && (padStatus == PAD_STATUS_FLYING) && (aircraftState != AIRCRAFT_TAKEOFF);
		bool hitBuilding = false;

		if (!loadingUnits && checkCollisions) {
			const std::map<boost::int64_t, CUnit*>& nearUnits = qf->GetUnitsExactStable(pos, owner->radius + 6);

			for (std::map<boost::int64_t, CUnit*>::const_iterator ui = nearUnits.begin(); ui != nearUnits.end(); ++ui) {
				CUnit* unit = ui->second;

				if (unit->StableTransporter() != NULL) // PROBLEM
					continue;

				const float sqDist = (pos - unit->StablePos()).SqLength();
				const float totRad = owner->radius + unit->StableRadius();

				if (sqDist <= 0.1f || sqDist >= (totRad * totRad))
					continue;

				const float dist = math::sqrt(sqDist);
				const float3 dif = (pos - unit->StablePos()).Normalize();

				if (unit->StableMass() >= CSolidObject::DEFAULT_MASS || unit->StableImmobile()) {
					owner->Move3D(-dif * (dist - totRad), true);
					owner->speed *= 0.99f;

					hitBuilding = true;
				} else {
					const float part = owner->mass / (owner->mass + unit->StableMass());

					owner->Move3D(-dif * (dist - totRad) * (1.0f - part), true);
					owner->QueMoveUnit(unit, dif * (dist - totRad) * (part), true, false);

					const float colSpeed = -owner->speed.dot(dif) + unit->StableSpeed().dot(dif);

					owner->speed += (dif * colSpeed * (1.0f - part));
					owner->QueChangeSpeed(unit, -(dif * colSpeed * (part)), 1.0f);
				}
			}
		}

		if (hitBuilding && owner->IsCrashing()) {
			owner->KillUnit(true, false, 0);
			return true;
		}

		if (pos.x < 0.0f) {
			owner->Move1D(0.6f, 0, true);
		} else if (pos.x > float3::maxxpos) {
			owner->Move1D(-0.6f, 0, true);
		}

		if (pos.z < 0.0f) {
			owner->Move1D(0.6f, 2, true);
		} else if (pos.z > float3::maxzpos) {
			owner->Move1D(-0.6f, 2, true);
		}

		return true;
	}

	return false;
}
