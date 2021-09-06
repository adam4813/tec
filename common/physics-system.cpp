#include "physics-system.hpp"

// #include "physics/physics-debug-drawer.hpp"
#include <BulletCollision/Gimpact/btGImpactCollisionAlgorithm.h>
#include <BulletCollision/Gimpact/btGImpactShape.h>
#include <glm/gtc/matrix_transform.hpp>

#include "components/collision-body.hpp"
#include "components/transforms.hpp"
#include "components/velocity.hpp"
#include "entity.hpp"
#include "events.hpp"
#include "multiton.hpp"

namespace tec {
using CollisionBodyMap = Multiton<eid, CollisionBody*>;
// #ifdef CLIENT_STANDALONE
// 	PhysicsDebugDrawer debug_drawer;
// #endif

PhysicsSystem::PhysicsSystem() {
	this->last_rayvalid = false;
	this->collisionConfiguration = new btDefaultCollisionConfiguration();
	this->dispatcher = new btCollisionDispatcher(this->collisionConfiguration);
	this->broadphase = new btDbvtBroadphase();
	this->solver = new btSequentialImpulseConstraintSolver();
	this->dynamicsWorld =
			new btDiscreteDynamicsWorld(this->dispatcher, this->broadphase, this->solver, this->collisionConfiguration);
	this->dynamicsWorld->setGravity(btVector3(0, -10.0, 0));

	btGImpactCollisionAlgorithm::registerAlgorithm(this->dispatcher);

	// #ifdef CLIENT_STANDALONE
	// 		debug_drawer.setDebugMode(btIDebugDraw::DBG_DrawWireframe |
	// btIDebugDraw::DBG_DrawAabb); 		this->dynamicsWorld->setDebugDrawer(&debug_drawer); #endif
}

PhysicsSystem::~PhysicsSystem() {
	if (this->dynamicsWorld != nullptr) {
		delete this->dynamicsWorld;
	}
	if (this->solver != nullptr) {
		delete this->solver;
	}
	if (this->collisionConfiguration != nullptr) {
		delete this->collisionConfiguration;
	}
	if (this->dispatcher != nullptr) {
		delete this->dispatcher;
	}
	if (this->broadphase != nullptr) {
		delete this->broadphase;
	}
}

std::set<eid> PhysicsSystem::Update(const double delta, const GameState& state) {
	ProcessCommandQueue();
	EventQueue<MouseBtnEvent>::ProcessEventQueue();
	EventQueue<EntityCreated>::ProcessEventQueue();
	EventQueue<EntityDestroyed>::ProcessEventQueue();

	for (auto itr = CollisionBodyMap::Begin(); itr != CollisionBodyMap::End(); ++itr) {
		const eid& entity_id = itr->first;

		CollisionBody* collidable = itr->second;
		if (!collidable) {
			continue;
		}
		// fill in the transform for our collidable from the current state
		auto position_iter = state.positions.find(entity_id);
		if (position_iter != state.positions.end()) {
			glm::vec3 position = position_iter->second.value;
			if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
				collidable->motion_state.transform.setOrigin(btVector3(position.x, position.y, position.z));
			}
		}
		else {
			// no position! that's not good
			// wait to add the physics body to the world for now
			continue;
		}
		auto orientation_iter = state.orientations.find(entity_id);
		if (orientation_iter != state.orientations.end()) {
			glm::quat orientation = orientation_iter->second.value;
			if (std::isfinite(orientation.x) && std::isfinite(orientation.y) && std::isfinite(orientation.z)
				&& std::isfinite(orientation.w)) {
				collidable->motion_state.transform.setRotation(
						btQuaternion(orientation.x, orientation.y, orientation.z, orientation.w));
			}
		}

		btRigidBody* body = this->bodies[entity_id];
		if (!body) {
			continue;
		}

		// handle changes to desired deactivation mode
		if (collidable->disable_deactivation) {
			body->forceActivationState(DISABLE_DEACTIVATION);
		}
		else if (body->getActivationState() == DISABLE_DEACTIVATION) {
			body->forceActivationState(ACTIVE_TAG);
		}

		// here we add the body to the world if it's not yet
		// this can later expand to handling multiple dynamics worlds if need be
		if (!collidable->in_world) {
			// TODO if we want to change any of these parameters later, we *might* have to remove the body from the world.
			// The Bullet documentation is rather unhelpful in this regard.
			// so for now we'll just update them when we put it in the world.

			// if the mass changed, update mass related parameters.
			if (collidable->mass != body->getInvMass()) {
				btVector3 fallInertia(0, 0, 0);
				collidable->shape->calculateLocalInertia(collidable->mass, fallInertia);
				body->setMassProps(collidable->mass, fallInertia);
				body->updateInertiaTensor();
				body->clearForces();
			}

			// prevent the simulation from rotating the object
			// this doesn't account for change after creation, once disabled there isn't a re-enable
			if (collidable->disable_rotation) {
				body->setAngularFactor(btVector3(0.0, 0, 0.0));
			}

			// snap the body to it's position when we add it
			body->setWorldTransform(collidable->motion_state.transform);
			collidable->in_world = true;
			this->dynamicsWorld->addRigidBody(body);
		}
		else {
			btTransform& body_transform = body->getWorldTransform();
			// simulation motion estimation lite
			// on the server, this doesn't really do anything
			// on the client however, this smooths out the motion between the local estimation and server state
			// this provides a crude but effective error correction until a better one is added ;)
			if (body_transform.getOrigin().distance(collidable->motion_state.transform.getOrigin()) > 0.01) {
				// FIXME there is a race condition outside of PhysicsSystem, where the position of the entity
				// is momentarily at origin (0,0,0) during entity creation (affects server)
				body->translate(0.5 * (collidable->motion_state.transform.getOrigin() - body_transform.getOrigin()));
				// use this to snap back to the current state, it's unpleasent without any form of restitution
				//body_transform.setOrigin(collidable->motion_state.transform.getOrigin());
			}
			// for now, just always update the orientation
			body_transform.setBasis(collidable->motion_state.transform.getBasis());
		}

		// always copy in the velocities from the state
		auto velocity_iter = state.velocities.find(entity_id);
		if (velocity_iter != state.velocities.end()) {
			const Velocity& vel = velocity_iter->second;
			if (std::isfinite(vel.linear.x) && std::isfinite(vel.linear.y) && std::isfinite(vel.linear.z)) {
				body->setLinearVelocity(vel.GetLinear() + body->getGravity());
			}
			if (std::isfinite(vel.angular.x) && std::isfinite(vel.angular.y) && std::isfinite(vel.angular.z)) {
				body->setAngularVelocity(vel.GetAngular());
			}
		}
	}

	// using a delta time here makes physics far less deterministic
	// this can be changed if it becomes a problem
	this->dynamicsWorld->stepSimulation(static_cast<btScalar>(delta), this->simulation_substeps);

	// build a set of entity IDs that changed this step
	std::set<eid> updated_entities;
	for (auto itr = CollisionBodyMap::Begin(); itr != CollisionBodyMap::End(); ++itr) {
		auto entity_id = itr->first;
		if (itr->second->motion_state.transform_updated) {
			updated_entities.insert(entity_id);
			itr->second->motion_state.transform_updated = false;
		}
	}
	return updated_entities;
}

glm::vec3 GetRayDirection(
		float mouse_x, float mouse_y, float screen_width, float screen_height, glm::mat4 view, glm::mat4 projection) {
	glm::vec4 ray_start_NDC(
			(mouse_x / screen_width - 0.5f) * 2.0f, (mouse_y / screen_height - 0.5f) * -2.0f, -1.0, 1.0f);
	glm::vec4 ray_end_NDC((mouse_x / screen_width - 0.5f) * 2.0f, (mouse_y / screen_height - 0.5f) * -2.0f, 0.0, 1.0f);

	glm::mat4 inverted_viewprojection = glm::inverse(projection * view);
	glm::vec4 ray_start_WORLD = inverted_viewprojection * ray_start_NDC;
	ray_start_WORLD /= ray_start_WORLD.w;
	glm::vec4 ray_end_WORLD = inverted_viewprojection * ray_end_NDC;
	ray_end_WORLD /= ray_end_WORLD.w;
	glm::vec3 ray_direction_WORLD(ray_end_WORLD - ray_start_WORLD);
	ray_direction_WORLD = glm::normalize(ray_direction_WORLD);
	return ray_direction_WORLD;
}

eid PhysicsSystem::RayCastMousePick(
		eid source_entity, double mouse_x, double mouse_y, float screen_width, float screen_height) {
	if (source_entity == 0) {
		return 0;
	}
	auto body_iter = this->bodies.find(source_entity);
	if (this->bodies.find(source_entity) == this->bodies.end()) {
		return 0;
	}
	this->last_rayvalid = false;
	this->last_entity_hit = 0;

	CollisionBody* body = static_cast<CollisionBody*>(body_iter->second->getUserPointer());
	auto pos = body->motion_state.transform.getOrigin();
	glm::vec3 position(pos.x(), pos.y(), pos.z());
	auto rot = body->motion_state.transform.getRotation();
	glm::quat orientation(rot.w(), rot.x(), rot.y(), rot.z());

	if (screen_height == 0.0f) {
		return 0;
	}
	// TODO: This could be pulled from something but it seems unlikely to change.
	static glm::mat4 projection = glm::perspective(glm::radians(45.0f), screen_width / screen_height, -1.0f, 300.0f);
	glm::mat4 view = glm::inverse(glm::translate(glm::mat4(1.0), position) * glm::mat4_cast(orientation));

	glm::vec3 world_direction = position
								- GetRayDirection(
										  static_cast<float>(mouse_x),
										  static_cast<float>(mouse_y),
										  screen_width,
										  screen_height,
										  view,
										  projection)
										  * 100.0f;

	btVector3 from(position.x, position.y, position.z), to(world_direction.x, world_direction.y, world_direction.z);
	this->last_rayfrom = from;
	btDynamicsWorld::AllHitsRayResultCallback ray_result_callback(from, to);
	this->dynamicsWorld->rayTest(from, to, ray_result_callback);
	if (ray_result_callback.hasHit()) {
		int collision_object_count = ray_result_callback.m_collisionObjects.size();
		double lastfrac = 1.1;
		int hit_entity_index = collision_object_count;
		eid hit_entity = 0;
		for (int i = 0; i < collision_object_count; i++) {
			eid entity = 0;
			double frc = ray_result_callback.m_hitFractions.at(i);
			const CollisionBody* colbody =
					(const CollisionBody*)ray_result_callback.m_collisionObjects.at(i)->getUserPointer();
			if (!colbody) {
				continue;
			}
			entity = colbody->entity_id;
			if (entity && entity != source_entity) {
				if (frc < lastfrac) {
					hit_entity = entity;
					hit_entity_index = i;
					lastfrac = frc;
				}
			}
		}
		this->last_entity_hit = hit_entity;
		if (hit_entity_index < collision_object_count) {
			this->last_raypos = ray_result_callback.m_hitPointWorld.at(hit_entity_index);
			this->last_raynorm = ray_result_callback.m_hitNormalWorld.at(hit_entity_index);
			this->last_raydist = last_rayfrom.distance(this->last_raypos);
			this->last_rayvalid = true;
		}
	}
	return this->last_entity_hit;
}

eid PhysicsSystem::RayCastIgnore(eid source_entity, eid ignore_entity) {
	last_rayvalid = false;
	glm::vec3 position;
	if (Entity(source_entity).Has<Position>()) {
		position = (Entity(source_entity).Get<Position>())->value;
	}
	glm::quat orientation;
	if (Entity(source_entity).Has<Orientation>()) {
		orientation = (Entity(source_entity).Get<Orientation>())->value;
	}
	auto fv = position + glm::rotate(orientation, FORWARD_VECTOR * 300.f);
	btVector3 from(position.x, position.y, position.z), to(fv.x, fv.y, fv.z);
	last_rayfrom = from;
	btDynamicsWorld::AllHitsRayResultCallback cr(from, to);
	this->dynamicsWorld->rayTest(from, to, cr);
	if (cr.hasHit()) {
		int mx = cr.m_collisionObjects.size();
		double lastfrac = 1.1;
		int hc = mx;
		eid entity_hit = 0;
		for (int i = 0; i < mx; i++) {
			eid entity = 0;
			double frc = cr.m_hitFractions.at(i);
			const CollisionBody* coll = (const CollisionBody*)cr.m_collisionObjects.at(i)->getUserPointer();
			if (!coll)
				continue;
			entity = coll->entity_id;
			if (entity && entity != source_entity && entity != ignore_entity) {
				if (frc < lastfrac) {
					entity_hit = entity;
					hc = i;
					lastfrac = frc;
				}
			}
		}
		if (hc < mx) {
			last_raypos = cr.m_hitPointWorld.at(hc);
			last_raynorm = cr.m_hitNormalWorld.at(hc);
			last_raydist = last_rayfrom.distance(last_raypos);
			last_rayvalid = true;
			return entity_hit;
		}
	}
	return 0;
}

void PhysicsSystem::DebugDraw() {
	this->dynamicsWorld->debugDrawWorld();

	// #ifdef CLIENT_STANDALONE
	// 		debug_drawer.UpdateVertexBuffer();
	// #endif
}

void PhysicsSystem::SetGravity(const unsigned int entity_id, const btVector3& f) {
	if (this->bodies.find(entity_id) != this->bodies.end()) {
		this->bodies[entity_id]->setGravity(f);
	}
}

void PhysicsSystem::SetNormalGravity(const unsigned int entity_id) {
	if (this->bodies.find(entity_id) != this->bodies.end()) {
		this->bodies[entity_id]->setGravity(this->dynamicsWorld->getGravity());
	}
}

bool PhysicsSystem::AddRigidBody(CollisionBody* collision_body) {
	eid entity_id = collision_body->entity_id;

	RemoveRigidBody(entity_id);

	if (!collision_body->shape) {
		return false;
	}

	btVector3 fallInertia(0, 0, 0);
	if (collision_body->mass > 0.0) {
		if (collision_body->shape) {
			collision_body->shape->calculateLocalInertia(collision_body->mass, fallInertia);
		}
	}

	btRigidBody::btRigidBodyConstructionInfo fallRigidBodyCI(
			collision_body->mass, &collision_body->motion_state, collision_body->shape.get(), fallInertia);
	auto body = new btRigidBody(fallRigidBodyCI);

	if (!body) {
		return false;
	}

	this->bodies[entity_id] = body;

	body->setUserPointer(collision_body);
	return true;
}

void PhysicsSystem::RemoveRigidBody(eid entity_id) {
	if (this->bodies.find(entity_id) != this->bodies.end()) {
		if (this->bodies.at(entity_id)) {
			this->dynamicsWorld->removeRigidBody(this->bodies.at(entity_id));
			delete this->bodies.at(entity_id);
		}
	}
}

void PhysicsSystem::On(eid, std::shared_ptr<MouseBtnEvent> data) {
	if (data->action == MouseBtnEvent::DOWN) {
		if (this->last_entity_hit) {
			std::shared_ptr<MouseClickEvent> mce_event = std::make_shared<MouseClickEvent>();
			mce_event->button = data->button;
			mce_event->ray_distance = this->last_raydist;
			mce_event->ray_hit_point_world =
					glm::vec3(this->last_raypos.getX(), this->last_raypos.getY(), this->last_raypos.getZ());
			EventSystem<MouseClickEvent>::Get()->Emit(this->last_entity_hit, mce_event);
		}
	}
}

void PhysicsSystem::On(eid, std::shared_ptr<EntityCreated> data) {
	eid entity_id = data->entity.id();
	for (int i = 0; i < data->entity.components_size(); ++i) {
		const proto::Component& comp = data->entity.components(i);
		switch (comp.component_case()) {
		case proto::Component::kCollisionBody:
		{
			CollisionBody* collision_body = new CollisionBody();
			collision_body->In(comp);
			CollisionBodyMap::Set(entity_id, collision_body);
			collision_body->entity_id = entity_id;
			AddRigidBody(collision_body);
			break;
		}
		case proto::Component::kPosition:
		{
			Position* position = new Position();
			position->In(comp);
			Multiton<eid, Position*>::Set(entity_id, position);
			break;
		}
		case proto::Component::kOrientation:
		{
			Orientation* orientation = new Orientation();
			orientation->In(comp);
			Multiton<eid, Orientation*>::Set(entity_id, orientation);
			break;
		};
		default: break;
		}
	}
}

void PhysicsSystem::On(eid entity_id, std::shared_ptr<EntityDestroyed> data) {
	CollisionBodyMap::Remove(entity_id);
	RemoveRigidBody(entity_id);
	this->bodies.erase(entity_id); // There isn't a chance it will be re-added.
	PositionMap::Remove(entity_id);
	OrientationMap::Remove(entity_id);
}

Position PhysicsSystem::GetPosition(eid entity_id) {
	auto body_iter = this->bodies.find(entity_id);
	if (body_iter != this->bodies.end() && body_iter->second) {
		//CollisionBody* body = static_cast<CollisionBody*>(body_iter->second->getUserPointer());
		auto pos = body_iter->second->getWorldTransform().getOrigin();
		return glm::vec3(pos.x(), pos.y(), pos.z());
	}
	return glm::vec3();
}

Orientation PhysicsSystem::GetOrientation(eid entity_id) {
	auto body_iter = this->bodies.find(entity_id);
	if (body_iter != this->bodies.end() && body_iter->second) {
		//CollisionBody* body = static_cast<CollisionBody*>(body_iter->second->getUserPointer());
		auto rot = body_iter->second->getWorldTransform().getRotation();
		return glm::quat(rot.w(), rot.x(), rot.y(), rot.z());
	}
	return glm::quat();
}
} // namespace tec
