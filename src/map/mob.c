/**
 * This file is part of Hercules.
 * http://herc.ws - http://github.com/HerculesWS/Hercules
 *
 * Copyright (C) 2012-2025 Hercules Dev Team
 * Copyright (C) Athena Dev Teams
 *
 * Hercules is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define HERCULES_CORE

#include "config/core.h" // AUTOLOOT_DISTANCE, DBPATH, DEFTYPE_MAX, DEFTYPE_MIN, RENEWAL_DROP, RENEWAL_EXP
#include "mob.h"

#include "map/atcommand.h"
#include "map/battle.h"
#include "map/clif.h"
#include "map/date.h"
#include "map/elemental.h"
#include "map/guild.h"
#include "map/homunculus.h"
#include "map/intif.h"
#include "map/itemdb.h"
#include "map/log.h"
#include "map/map.h"
#include "map/mercenary.h"
#include "map/npc.h"
#include "map/party.h"
#include "map/path.h"
#include "map/pc.h"
#include "map/pet.h"
#include "map/quest.h"
#include "map/script.h"
#include "map/skill.h"
#include "map/status.h"
#include "map/achievement.h"
#include "common/HPM.h"
#include "common/cbasetypes.h"
#include "common/conf.h"
#include "common/db.h"
#include "common/ers.h"
#include "common/memmgr.h"
#include "common/msgtable.h"
#include "common/nullpo.h"
#include "common/random.h"
#include "common/showmsg.h"
#include "common/socket.h"
#include "common/strlib.h"
#include "common/timer.h"
#include "common/utils.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct mob_interface mob_s;
struct mob_interface *mob;

#define ACTIVE_AI_RANGE 2 //Distance added on top of 'AREA_SIZE' at which mobs enter active AI mode.

#define IDLE_SKILL_INTERVAL 10 //Active idle skills should be triggered every 1 second (1000/MIN_MOBTHINKTIME)

// Probability for mobs far from players from doing their IDLE skill. (rate of 1000 minute)
// in Aegis, this is 100% for mobs that have been activated by players and none otherwise.
#define MOB_LAZYSKILLPERC(md) (md->state.spotted?1000:0)
// Move probability for mobs away from players (rate of 1000 minute)
// in Aegis, this is 100% for mobs that have been activated by players and none otherwise.
#define MOB_LAZYMOVEPERC(md) ((md)->state.spotted?1000:0)
#define MOB_MAX_CASTTIME (10 * 60 * 1000) // Maximum cast time for monster skills. (10 minutes)
#define MOB_MAX_DELAY (24*3600*1000)
#define MAX_MINCHASE 30 //Max minimum chase value to use for mobs.
#define RUDE_ATTACKED_COUNT 2 //After how many rude-attacks should the skill be used?

//Dynamic item drop ratio database for per-item drop ratio modifiers overriding global drop ratios.
#define MAX_ITEMRATIO_MOBS 10
struct item_drop_ratio {
	int drop_ratio;
	int mob_id[MAX_ITEMRATIO_MOBS];
};
static struct item_drop_ratio *item_drop_ratio_db[MAX_ITEMDB];
static struct DBMap *item_drop_ratio_other_db = NULL;

static struct eri *item_drop_ers; //For loot drops delay structures.
static struct eri *item_drop_list_ers;

static struct mob_db *mob_db(int index)
{
	if (index < 0 || index > MAX_MOB_DB || mob->db_data[index] == NULL)
		return mob->dummy;
	return mob->db_data[index];
}
static struct mob_chat *mob_chat(short id)
{
	if(id <= 0 || id > MAX_MOB_CHAT || mob->chat_db[id] == NULL)
		return NULL;
	return mob->chat_db[id];
}

/*==========================================
 * Mob is searched with a name.
 *------------------------------------------*/
static int mobdb_searchname(const char *str)
{
	int i;

	nullpo_ret(str);
	for(i=0;i<=MAX_MOB_DB;i++){
		struct mob_db *monster = mob->db(i);
		if(monster == mob->dummy) //Skip dummy mobs.
			continue;
		if(strcmpi(monster->name,str)==0 || strcmpi(monster->jname,str)==0)
			return i;
		if(battle_config.case_sensitive_aegisnames && strcmp(monster->sprite,str)==0)
			return i;
		if(!battle_config.case_sensitive_aegisnames && strcasecmp(monster->sprite,str)==0)
			return i;
	}

	return 0;
}
static int mobdb_searchname_array_sub(struct mob_db *monster, const char *str, int flag)
{

	nullpo_ret(monster);
	if (monster == mob->dummy)
		return 1;
	if(!monster->base_exp && !monster->job_exp && monster->spawn[0].qty < 1)
		return 1; // Monsters with no base/job exp and no spawn point are, by this criteria, considered "slave mobs" and excluded from search results
	nullpo_ret(str);
	if( !flag ) {
		if(stristr(monster->jname,str))
			return 0;
		if(stristr(monster->name,str))
			return 0;
	} else {
		if(strcmpi(monster->jname,str) == 0)
			return 0;
		if(strcmpi(monster->name,str) == 0)
			return 0;
	}
	if (battle_config.case_sensitive_aegisnames)
		return strcmp(monster->sprite,str);
	return strcasecmp(monster->sprite,str);
}

/*==========================================
 *              MvP Tomb [GreenBox]
 *------------------------------------------*/
 /// Creates a timer to spawn a tomb
 /// @param nd : The tomb
static void mvptomb_spawn_delayed(struct npc_data *nd)
{
	nullpo_retv(nd);

	if (nd->u.tomb.spawn_timer != INVALID_TIMER)
		timer->delete(nd->u.tomb.spawn_timer, mob->mvptomb_delayspawn);

	nd->u.tomb.spawn_timer = timer->add(timer->gettick() + battle_config.mvp_tomb_spawn_delay, mob->mvptomb_delayspawn, nd->bl.id, 0);
}

/// Spawns a tomb after the delay has ended
/// @param tid : Timer id
/// @param tick : current tick
/// @param id : NPC Id
/// @param data : 0
static int mvptomb_delayspawn(int tid, int64 tick, int id, intptr_t data)
{
	struct npc_data *nd = map->id2nd(id);

	if (nd == NULL)
		return 0;

	if (nd->u.tomb.spawn_timer != tid) {
		ShowError("mvptomb_delay_spawn: Timer mismatch: %d != %d\n", tid, nd->u.tomb.spawn_timer);
		return 0;
	}

	nd->u.tomb.spawn_timer = INVALID_TIMER;

	// Sets view data to make the tomb visible and notifies client
	status->set_viewdata(&nd->bl, nd->class_);
	clif->spawn(&(nd->bl));

	return 0;
}

static void mvptomb_create(struct mob_data *md, char *killer, time_t time)
{
	struct npc_data *nd;

	nullpo_retv(md);
	if ( md->tomb_nid )
		mob->mvptomb_destroy(md);

	nd = npc->create_npc(TOMB, md->bl.m, md->bl.x, md->bl.y, md->ud.dir, MOB_TOMB);
	md->tomb_nid = nd->bl.id;

	safestrncpy(nd->name, msg_txt(MSGTBL_TOMB), sizeof(nd->name)); // "Tomb"

	nd->u.tomb.md = md;
	nd->u.tomb.kill_time = time;
	nd->u.tomb.spawn_timer = INVALID_TIMER;

	if (killer)
		safestrncpy(nd->u.tomb.killer_name, killer, NAME_LENGTH);
	else
		nd->u.tomb.killer_name[0] = '\0';

	map->addnpc(nd->bl.m, nd);
	map->addblock(&nd->bl);

	// Tomb npc is created but not yet visible, we set view data and spawn it after some time
	mob->mvptomb_spawn_delayed(nd);
}

static void mvptomb_destroy(struct mob_data *md)
{
	struct npc_data *nd;

	nullpo_retv(md);
	if ( (nd = map->id2nd(md->tomb_nid)) ) {
		int16 m = nd->bl.m;

		struct s_mapiterator *iter = mapit_geteachpc();
		for (struct map_session_data *sd = BL_UCAST(BL_PC, mapit->first(iter)); mapit->exists(iter); sd = BL_UCAST(BL_PC, mapit->next(iter))) {
			if (sd->npc_id == nd->bl.id) {
				sd->state.npc_unloaded = 1;
			}
		}
		mapit->free(iter);

		clif->clearunit_area(&nd->bl,CLR_OUTSIGHT);

		map->delblock(&nd->bl);

		int i = 0;
		ARR_FIND( 0, map->list[m].npc_num, i, map->list[m].npc[i] == nd );
		if( !(i == map->list[m].npc_num) ) {
			map->list[m].npc_num--;
			map->list[m].npc[i] = map->list[m].npc[map->list[m].npc_num];
			map->list[m].npc[map->list[m].npc_num] = NULL;
		}

		if (nd->u.tomb.spawn_timer != INVALID_TIMER)
			timer->delete(nd->u.tomb.spawn_timer, mob->mvptomb_delayspawn);

		map->deliddb(&nd->bl);

		aFree(nd);
	}

	md->tomb_nid = 0;
}

/*==========================================
 * Founds up to N matches. Returns number of matches [Skotlex]
 *------------------------------------------*/
static int mobdb_searchname_array(struct mob_db **data, int size, const char *str, int flag)
{
	int count = 0, i;
	struct mob_db* monster;
	nullpo_ret(data);
	for(i=0;i<=MAX_MOB_DB;i++){
		monster = mob->db(i);
		if (monster == mob->dummy || mob->is_clone(i) ) //keep clones out (or you leak player stats)
			continue;
		if (!mob->db_searchname_array_sub(monster, str, flag)) {
			if (count < size)
				data[count] = monster;
			count++;
		}
	}
	return count;
}

/*==========================================
 * Id Mob is checked.
 *------------------------------------------*/
static int mobdb_checkid(const int id)
{
	if (mob->db(id) == mob->dummy)
		return 0;
	if (mob->is_clone(id)) //checkid is used mostly for random ID based code, therefore clone mobs are out of the question.
		return 0;
	return id;
}

/*==========================================
 * Returns the view data associated to this mob class.
 *------------------------------------------*/
static struct view_data *mob_get_viewdata(int class_)
{
	if (mob->db(class_) == mob->dummy)
		return 0;
	return &mob->db(class_)->vd;
}

/**
 * Create unique view data associated to a spawned monster.
 * @param md: Mob to adjust
 */
static void mob_set_dynamic_viewdata(struct mob_data *md)
{
	nullpo_retv(md);

	// If it is a valid monster and it has not already been created
	if (md->vd_changed)
		return;

	// Allocate a dynamic entry
	struct view_data *vd = (struct view_data *)aMalloc(sizeof(struct view_data));
	// Copy the current values
	memcpy(vd, md->vd, sizeof(struct view_data));
	// Update the pointer to the new entry
	md->vd = vd;
	// Flag it as changed so it is freed later on
	md->vd_changed = true;
}

/**
 * Free any view data associated to a spawned monster.
 * @param md: Mob to free
 */
static void mob_free_dynamic_viewdata(struct mob_data *md)
{
	nullpo_retv(md);

	// If it is a valid monster and it has already been allocated
	if (!md->vd_changed)
		return;

	// Free it
	aFree(md->vd);
	// Remove the reference
	md->vd = NULL;
	// Unflag it as changed
	md->vd_changed = false;
}

/*==========================================
 * Cleans up mob-spawn data to make it "valid"
 *------------------------------------------*/
static int mob_parse_dataset(struct spawn_data *data)
{
	size_t len;

	nullpo_ret(data);
	if ((!mob->db_checkid(data->class_) && !mob->is_clone(data->class_)) || !data->num)
		return 0;

	if( ( len = strlen(data->eventname) ) > 0 )
	{
		if( data->eventname[len-1] == '"' )
			data->eventname[len-1] = '\0'; //Remove trailing quote.
		if( data->eventname[0] == '"' ) //Strip leading quotes
			memmove(data->eventname, data->eventname+1, len-1);
	}

	if (strcmp(data->name, DEFAULT_MOB_NAME) == 0)
		safestrncpy(data->name, mob->db(data->class_)->name, sizeof(data->name));
	else if (strcmp(data->name, DEFAULT_MOB_JNAME) == 0)
		safestrncpy(data->name, mob->db(data->class_)->jname, sizeof(data->name));

	return 1;
}

/**
 * Generates basic mob data by using the passed spawn data.
 *
 * @param data The mobs spawn data.
 * @param npc_id If spawned by NPC script, this holds the ID of the invoking NPC.
 * @return The generated mob data, later used to spawn the mob.
 *
 **/
static struct mob_data *mob_spawn_dataset(struct spawn_data *data, int npc_id)
{
	nullpo_retr(NULL, data);

	struct mob_data *md = (struct mob_data *)aCalloc(1, sizeof(struct mob_data));

	memcpy(md->name, data->name, NAME_LENGTH);
	md->bl.id = npc->get_new_npc_id();
	md->bl.type = BL_MOB;
	md->bl.m = data->m;
	md->bl.x = data->x;
	md->bl.y = data->y;
	md->class_ = data->class_;
	md->state.boss = data->state.boss;
	md->db = mob->db(md->class_);
	md->npc_id = npc_id;
	md->spawn_timer = INVALID_TIMER;
	md->deletetimer = INVALID_TIMER;
	md->skill_idx = -1;

	if (data->level > 0 && data->level <= MAX_LEVEL)
		md->level = data->level;

	if (data->state.ai > 0)
		md->special_state.ai = data->state.ai;

	if (data->state.size > 0)
		md->special_state.size = data->state.size;

	if (data->eventname[0] != '\0' && strlen(data->eventname) >= 4)
		memcpy(md->npc_event, data->eventname, 50);

	if ((md->db->status.mode & MD_LOOTER) == MD_LOOTER)
		md->lootitem = (struct item *)aCalloc(LOOTITEM_SIZE, sizeof(struct item));

	status->set_viewdata(&md->bl, md->class_);
	status->change_init(&md->bl);
	unit->dataset(&md->bl);
	map->addiddb(&md->bl);
	return md;
}

/*==========================================
 * Fetches a random mob_id [Skotlex]
 * type: Where to fetch from:
 * 0: dead branch list
 * 1: poring list
 * 2: bloody branch list
 * flag:
 * &1: Apply the summon success chance found in the list (otherwise get any monster from the db)
 * &2: Apply a monster check level.
 * &4: Selected monster should not be a boss type
 * &8: Selected monster must have normal spawn.
 * lv: Mob level to check against
 *------------------------------------------*/
static int mob_get_random_id(enum mob_groups type, int flag, int lv)
{
	Assert_ret(type >= 0 && type < MOBG_MAX_GROUP);

	struct mob_db *monster;
	int i = 0;
	int class_ = 0;

	do {
		if (type) {
			const int idx = rnd() % VECTOR_LENGTH(mob->mob_groups[type]);
			class_ = VECTOR_INDEX(mob->mob_groups[type], idx);
		} else {
			class_ = rnd() % MAX_MOB_DB;
		}
		monster = mob->db(class_);
	} while ((monster == mob->dummy ||
		mob->is_clone(class_) ||
		(flag&1 && monster->summonper[type] <= rnd() % 1000000) ||
		(flag&2 && lv < monster->lv) ||
		(flag&4 && monster->status.mode&MD_BOSS) ||
		(flag&8 && monster->spawn[0].qty < 1)
	) && (i++) < MAX_MOB_DB);

	if (i >= MAX_MOB_DB) // no suitable monster found, use fallback
		class_ = MOBID_PORING;
	return class_;
}

/*==========================================
 * Kill Steal Protection [Zephyrus]
 *------------------------------------------*/
static bool mob_ksprotected(struct block_list *src, struct block_list *target)
{
	struct block_list *s_bl, *t_bl;
	struct map_session_data
		*sd,    // Source
		*pl_sd, // Owner
		*t_sd;  // Mob Target
	struct mob_data *md;
	int64 tick = timer->gettick();
	char output[128];

	if( !battle_config.ksprotection )
		return false; // KS Protection Disabled

	if (status->isdead(target) != 0)
		return false; // Target is dead.

	if( !(md = BL_CAST(BL_MOB,target)) )
		return false; // Target is not MOB

	if( (s_bl = battle->get_master(src)) == NULL )
		s_bl = src;

	if( !(sd = BL_CAST(BL_PC,s_bl)) )
		return false; // Master is not PC

	t_bl = map->id2bl(md->target_id);
	if( !t_bl || (s_bl = battle->get_master(t_bl)) == NULL )
		s_bl = t_bl;

	t_sd = BL_CAST(BL_PC,s_bl);

	do {
		struct status_change_entry *sce;
		if( map->list[md->bl.m].flag.allowks || map_flag_ks(md->bl.m) )
			return false; // Ignores GVG, PVP and AllowKS map flags

		if( md->db->mexp || md->master_id )
			return false; // MVP, Slaves mobs ignores KS

		if( (sce = md->sc.data[SC_KSPROTECTED]) == NULL )
			break; // No KS Protected

		if( sd->bl.id == sce->val1 || // Same Owner
			(sce->val2 == KSPROTECT_PARTY && sd->status.party_id && sd->status.party_id == sce->val3) || // Party KS allowed
			(sce->val2 == KSPROTECT_GUILD && sd->status.guild_id && sd->status.guild_id == sce->val4) ) // Guild KS allowed
			break;

		if( t_sd && (
			(sce->val2 == KSPROTECT_SELF && sce->val1 != t_sd->bl.id) ||
			(sce->val2 == KSPROTECT_PARTY && sce->val3 && sce->val3 != t_sd->status.party_id) ||
			(sce->val2 == KSPROTECT_GUILD && sce->val4 && sce->val4 != t_sd->status.guild_id)) )
			break;

		if( (pl_sd = map->id2sd(sce->val1)) == NULL || pl_sd->bl.m != md->bl.m )
			break;

		if( !pl_sd->state.noks )
			return false; // No KS Protected, but normal players should be protected too

		// Message to KS
		if( DIFF_TICK(sd->ks_floodprotect_tick, tick) <= 0 )
		{
			sprintf(output, msg_sd(sd, MSGTBL_KS_WARNING_OWNER), pl_sd->status.name); // [KS Warning!! - Owner : %s]
			clif_disp_onlyself(sd, output);

			sd->ks_floodprotect_tick = tick + 2000;
		}

		// Message to Owner
		if( DIFF_TICK(pl_sd->ks_floodprotect_tick, tick) <= 0 )
		{
			sprintf(output, msg_sd(pl_sd, MSGTBL_KS_WARNING_PLAYER), sd->status.name); // [Watch out! %s is trying to KS you!]
			clif_disp_onlyself(pl_sd, output);

			pl_sd->ks_floodprotect_tick = tick + 2000;
		}

		return true;
	} while(0);

	status->change_start(NULL, target, SC_KSPROTECTED, 10000, sd->bl.id, sd->state.noks,
	                     sd->status.party_id, sd->status.guild_id, battle_config.ksprotection, SCFLAG_NONE, 0);

	return false;
}

/**
 * Prepares a mob's spawn data.
 *
 * @param bl The invoking character's block list.
 * @param m The ID of the map where the mob should be spawned.
 * @param x The x coordinate where the mob should be spawned.
 * @param y The y coordinate where the mob should be spawned.
 * @param mobname The mob's display name.
 * @param class_ The mob's ID in database.
 * @param event The name of the event which should be executed when the mob is killed.
 * @param size The mob's size.
 * @param ai The mob's AI.
 * @param npc_id If spawned by NPC script, this holds the ID of the invoking NPC.
 * @return The mob data generated by mob->spawn_dataset().
 *
 **/
static struct mob_data *mob_once_spawn_sub(struct block_list *bl, int16 m, int16 x, int16 y, const char *mobname, int class_,
					   const char *event, unsigned int size, unsigned int ai, int npc_id)
{
	struct spawn_data data;

	memset(&data, 0, sizeof(struct spawn_data));
	data.m = m;
	data.num = 1;
	data.class_ = class_;
	data.state.size = size;
	data.state.ai = ai;

	if (mobname != NULL)
		safestrncpy(data.name, mobname, sizeof(data.name));
	else if (battle_config.override_mob_names == 1)
		strcpy(data.name, DEFAULT_MOB_NAME);
	else
		strcpy(data.name, DEFAULT_MOB_JNAME);

	if (event != NULL)
		safestrncpy(data.eventname, event, sizeof(data.eventname));

	/** Locate spot next to player. **/
	if (bl != NULL && (x < 0 || y < 0))
		map->search_free_cell(bl, m, &x, &y, 1, 1, SFC_DEFAULT);

	/** If none found, pick random position on map. **/
	if (x <= 0 || x >= map->list[m].xs || y <= 0 || y >= map->list[m].ys)
		map->search_free_cell(NULL, m, &x, &y, -1, -1, SFC_XY_CENTER);

	data.x = x;
	data.y = y;

	if (mob->parse_dataset(&data) == 0)
		return NULL;

	return mob->spawn_dataset(&data, npc_id);
}

/**
 * Spawns a given amount of mobs.
 *
 * @param sd The invoking character.
 * @param m The ID of the map where the mob should be spawned.
 * @param x The x coordinate where the mob should be spawned.
 * @param y The y coordinate where the mob should be spawned.
 * @param mobname The mob's display name.
 * @param class_ The mob's ID in database.
 * @param amount The amount of mobs to spawn.
 * @param event The name of the event which should be executed when the mob is killed.
 * @param size The mob's size.
 * @param ai The mob's AI.
 * @return The last spawned mob's GID, or 0 if spawning failed.
 *
 **/
static int mob_once_spawn(struct map_session_data *sd, int16 m, int16 x, int16 y, const char *mobname, int class_,
			  int amount, const char *event, unsigned int size, unsigned int ai)
{
	bool no_guardian_data = false;

	if (ai > 0 && (ai & 0x200) == 0x200) {
		no_guardian_data = true;
		ai &= ~0x200;
	}

	if (m < 0 || amount <= 0)
		return 0;

	struct mob_data *md = NULL;

	for (int i = 0; i < amount; i++) {
		int mob_id = class_;

		if (mob_id < 0) {
			mob_id = mob->get_random_id(-class_ - 1, (battle_config.random_monster_checklv == 1) ? 3 : 1,
						    (sd != NULL) ? sd->status.base_level : 255);
		}

		md = mob->once_spawn_sub((sd != NULL) ? &sd->bl : NULL, m, x, y, mobname, mob_id, event, size, ai,
					 (sd != NULL) ? sd->npc_id : 0);

		if (md == NULL)
			continue;

		if (class_ == MOBID_EMPELIUM && !no_guardian_data) {
			struct guild_castle *gc = guild->mapindex2gc(map_id2index(m));

			if (gc != NULL) {
				struct guild *g = guild->search(gc->guild_id);

				md->guardian_data = (struct guardian_data*)aCalloc(1, sizeof(struct guardian_data));
				md->guardian_data->castle = gc;
				md->guardian_data->number = MAX_GUARDIANS;

				if (g != NULL)
					md->guardian_data->g = g;
				else if (gc->guild_id > 0) /// Guild not yet available, retry in 5s.
					timer->add(timer->gettick() + 5000, mob->spawn_guardian_sub, md->bl.id,
						   gc->guild_id);
			}
		}

		mob->spawn(md);

		if (class_ < 0 && battle_config.dead_branch_active == 1) {
			/// Behold Aegis' masterful decisions yet again...
			/// "I understand the "Aggressive" part, but the "Can Move" and "Can Attack" is just stupid" [Poki]
			sc_start4(NULL, &md->bl, SC_MODECHANGE, 100, 1, 0, MD_AGGRESSIVE|MD_CANATTACK|MD_CANMOVE|MD_ANGRY,
				  0, 60000, 0);
		}
	}

	return (md != NULL) ? md->bl.id : 0; /// ID of last spawned mob.
}

/*==========================================
 * Spawn mobs in the specified area.
 *------------------------------------------*/
static int mob_once_spawn_area(struct map_session_data *sd, int16 m, int16 x0, int16 y0, int16 x1, int16 y1, const char *mobname, int class_, int amount, const char *event, unsigned int size, unsigned int ai)
{
	int i, max, id = 0;
	int lx = -1, ly = -1;

	if (m < 0 || amount <= 0)
		return 0; // invalid input

	// normalize x/y coordinates
	if (x0 > x1)
		swap(x0, x1);
	if (y0 > y1)
		swap(y0, y1);

	// choose a suitable max. number of attempts
	max = (y1 - y0 + 1)*(x1 - x0 + 1)*3;
	if (max > 1000)
		max = 1000;

	// spawn mobs, one by one
	for (i = 0; i < amount; i++)
	{
		int x, y;
		int j = 0;

		// find a suitable map cell
		do {
			x = rnd()%(x1-x0+1)+x0;
			y = rnd()%(y1-y0+1)+y0;
			j++;
		} while (map->getcell(m, NULL, x, y, CELL_CHKNOPASS) && j < max);

		if (j == max)
		{// attempt to find an available cell failed
			if (lx == -1 && ly == -1)
				return 0; // total failure

			// fallback to last good x/y pair
			x = lx;
			y = ly;
		}

		// record last successful coordinates
		lx = x;
		ly = y;

		id = mob->once_spawn(sd, m, x, y, mobname, class_, 1, event, size, ai);
	}

	return id; // id of last spawned mob
}

/**
 * Sets a guardian's guild data and liberates castle if couldn't retrieve guild data.
 * Required because the guild data may not be available at guardian spawn time.
 *
 * @param tid Required parameter for timer functions. Unused inside the function.
 * @param tick Required parameter for timer functions. Unused inside the function.
 * @param id The guardian mob's GID.
 * @param data The guild ID.
 * @return 1 on success, 0 on failure.
 *
 * @author Skotlex
 *
 **/
static int mob_spawn_guardian_sub(int tid, int64 tick, int id, intptr_t data)
{
	struct block_list *bl = map->id2bl(id);

	if (bl == NULL || bl->type != BL_MOB) /// It is possible mob was already removed from map when the castle has no owner. [Skotlex]
		return 0;

	struct mob_data *md = BL_UCAST(BL_MOB, bl);

	if (md->guardian_data == NULL)
		return 0;

	struct guild *g = guild->search((int)data);

	if (g == NULL) { /// Liberate castle, if the guild is not found this is an error! [Skotlex]
		ShowError("mob_spawn_guardian_sub: Couldn't load guild %d!\n", (int)data);

		/// Not sure this is the best way, but otherwise we'd be invoking this for ALL guardians spawned later on.
		if (md->class_ == MOBID_EMPELIUM) {
			md->guardian_data->g = NULL;

			if (md->guardian_data->castle->guild_id > 0) { /// Free castle up.
				ShowNotice("mob_spawn_guardian_sub: Clearing ownership of castle %d (%s).\n",
					   md->guardian_data->castle->castle_id,
					   md->guardian_data->castle->castle_name);
				guild->castledatasave(md->guardian_data->castle->castle_id, 1, 0);
			}
		} else {
			if (md->guardian_data->number >= 0 && md->guardian_data->number < MAX_GUARDIANS &&
			    md->guardian_data->castle->guardian[md->guardian_data->number].visible == 1)
				guild->castledatasave(md->guardian_data->castle->castle_id,
						      10 + md->guardian_data->number, 0);

			unit->free(&md->bl, CLR_OUTSIGHT); /// Remove guardian.
		}

		return 0;
	}

	if (guild->checkskill(g, GD_GUARDUP) > 0)
		status_calc_mob(md, SCO_NONE); /// Give bonuses.

	return 1;
}

/**
 * Summons a castle guardian mob.
 *
 * @param mapname The name of the map where the guardian should be spawned.
 * @param x The x coordinate where the guardian should be spawned.
 * @param y The y coordinate where the guardian should be spawned.
 * @param mobname The guardian's display name.
 * @param class_ The guardian's mob ID in database.
 * @param event The name of the event which should be executed when the guardian is killed.
 * @param guardian The guardian's index.
 * @param has_index If false, the guardian will be temporarily.
 * @param npc_id If spawned by NPC script, this holds the ID of the invoking NPC.
 * @return The spawned guardian's GID, or 0 if spawning failed.
 *
 * @author Valaris
 *
 **/
static int mob_spawn_guardian(const char *mapname, short x, short y, const char *mobname, int class_, const char *event,
			      int guardian, bool has_index, int npc_id)
{
	nullpo_ret(mapname);
	nullpo_ret(mobname);
	nullpo_ret(event);

	const int map_id = map->mapname2mapid(mapname);

	if (map_id == INDEX_NOT_FOUND) {
		ShowWarning("mob_spawn_guardian: Map [%s] not found.\n", mapname);
		return 0;
	}

	if ((x <= 0 || y <= 0) && map->search_free_cell(NULL, map_id, &x, &y, -1, -1, SFC_XY_CENTER) != 0) {
		ShowWarning("mob_spawn_guardian: Couldn't locate a spawn cell for guardian class %d (index %d) on castle map %s.\n",
			    class_, guardian, mapname);
		return 0;
	}

	if (class_ <= 0 && (class_ = mob->get_random_id(-class_ - 1, 1, 99)) == 0)
		return 0;

	if (!has_index) {
		guardian = -1;
	} else if (guardian < 0 || guardian >= MAX_GUARDIANS) {
		ShowError("mob_spawn_guardian: Invalid guardian index %d for guardian %d on castle map %s.\n",
			  guardian, class_, mapname);
		return 0;
	}

	struct spawn_data data;

	memset(&data, 0, sizeof(struct spawn_data));
	data.num = 1;
	data.m = map_id;
	data.class_ = class_;
	data.x = x;
	data.y = y;
	safestrncpy(data.name, mobname, sizeof(data.name));
	safestrncpy(data.eventname, event, sizeof(data.eventname));

	if (mob->parse_dataset(&data) == 0)
		return 0;

	struct guild_castle *gc = guild->mapname2gc(mapname);

	if (gc == NULL) {
		ShowError("mob_spawn_guardian: No castle set on map %s.\n", mapname);
		return 0;
	}

	struct guild *g = NULL;

	if (gc->guild_id == 0)
		ShowWarning("mob_spawn_guardian: Spawning guardian %d on a castle map %s with no guild.\n",
			    class_, mapname);
	else
		g = guild->search(gc->guild_id);

	if (has_index && gc->guardian[guardian].id != 0) { /// Check if guardian already exists, refuse to spawn if so.
		struct mob_data *md = map->id2md(gc->guardian[guardian].id);

		if (md != NULL && md->guardian_data != NULL && md->guardian_data->number == guardian) {
			ShowError("mob_spawn_guardian: Attempted to spawn guardian in position %d which already has a guardian on castle map %s.\n",
				  guardian, mapname);
			return 0;
		}
	}

	struct mob_data *md = mob->spawn_dataset(&data, npc_id);

	md->guardian_data = (struct guardian_data*)aCalloc(1, sizeof(struct guardian_data));
	md->guardian_data->number = guardian;
	md->guardian_data->castle = gc;

	if (has_index) { /// Permanent guardian.
		gc->guardian[guardian].id = md->bl.id;
	} else { /// Temporary guardian.
		int i;

		ARR_FIND(0, gc->temp_guardians_max, i, gc->temp_guardians[i] == 0);

		if (i == gc->temp_guardians_max) {
			++(gc->temp_guardians_max);
			RECREATE(gc->temp_guardians, int, gc->temp_guardians_max);
		}

		gc->temp_guardians[i] = md->bl.id;
	}

	if (g != NULL)
		md->guardian_data->g = g;
	else if (gc->guild_id > 0)
		timer->add(timer->gettick() + 5000, mob->spawn_guardian_sub, md->bl.id, gc->guild_id);

	mob->spawn(md);
	return md->bl.id;
}

/**
 * Spawn a mob with allegiance to the given battle group.
 *
 * @param mapname The name of the map where the mob should be spawned.
 * @param x The x coordinate where the mob should be spawned.
 * @param y The y coordinate where the mob should be spawned.
 * @param mobname The mob's display name.
 * @param class_ The mob's mob ID in database.
 * @param event The name of the event which should be executed when the mob is killed.
 * @param bg_id The battle group ID.
 * @param npc_id If spawned by NPC script, this holds the ID of the invoking NPC.
 * @return The spawned mob's GID, or 0 if spawning failed.
 *
 * @author Zephyrus
 *
 **/
static int mob_spawn_bg(const char *mapname, short x, short y, const char *mobname, int class_, const char *event,
			unsigned int bg_id, int npc_id)
{
	nullpo_ret(mapname);
	nullpo_ret(mobname);
	nullpo_ret(event);

	const int map_id = map->mapname2mapid(mapname);

	if (map_id == INDEX_NOT_FOUND) {
		ShowWarning("mob_spawn_bg: Map [%s] not found.\n", mapname);
		return 0;
	}

	if ((x <= 0 || y <= 0) && map->search_free_cell(NULL, map_id, &x, &y, -1, -1, SFC_XY_CENTER) != 0) {
		ShowWarning("mob_spawn_bg: Couldn't locate a spawn cell for guardian class %d (bg_id %u) on map %s.\n", class_, bg_id, mapname);
		return 0;
	}

	if (class_ <= 0 && (class_ = mob->get_random_id(-class_ - 1, 1, 99)) == 0)
		return 0;

	struct spawn_data data;

	memset(&data, 0, sizeof(struct spawn_data));
	data.num = 1;
	data.m = map_id;
	data.class_ = class_;
	data.x = x;
	data.y = y;
	safestrncpy(data.name, mobname, sizeof(data.name));
	safestrncpy(data.eventname, event, sizeof(data.eventname));

	if (mob->parse_dataset(&data) == 0)
		return 0;

	struct mob_data *md = mob->spawn_dataset(&data, npc_id);

	mob->spawn(md);
	md->bg_id = bg_id;
	return md->bl.id;
}

/*==========================================
 * Reachability to a Specification ID existence place
 * state indicates type of 'seek' mob should do:
 * - MSS_LOOT: Looking for item, path must be easy.
 * - MSS_RUSH: Chasing attacking player, path is complex
 * - MSS_FOLLOW: Initiative/support seek, path is complex
 *------------------------------------------*/
static int mob_can_reach(struct mob_data *md, struct block_list *bl, int range, int state)
{
	int easy = 0;

	nullpo_ret(md);
	nullpo_ret(bl);
	switch (state) {
		case MSS_RUSH:
		case MSS_FOLLOW:
			easy = 0; //(battle_config.mob_ai&0x1?0:1);
			break;
		case MSS_LOOT:
		default:
			easy = 1;
			break;
	}
	return unit->can_reach_bl(&md->bl, bl, range, easy, NULL, NULL);
}

/*==========================================
 * Links nearby mobs (supportive mobs)
 *------------------------------------------*/
static int mob_linksearch(struct block_list *bl, va_list ap)
{
	struct mob_data *md = NULL;
	int class_ = va_arg(ap, int);
	struct block_list *target = va_arg(ap, struct block_list *);
	int64 tick = va_arg(ap, int64);

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_MOB);
	md = BL_UCAST(BL_MOB, bl);

	if (md->class_ == class_ && DIFF_TICK(md->last_linktime, tick) < MIN_MOBLINKTIME
		&& !md->target_id)
	{
		md->last_linktime = tick;
		if (mob->can_reach(md,target,md->db->range2, MSS_FOLLOW)) {
			// Reachability judging
			md->target_id = target->id;
			md->min_chase=md->db->range3;
			return 1;
		}
	}

	return 0;
}

/*==========================================
 * mob spawn with delay (timer function)
 *------------------------------------------*/
static int mob_delayspawn(int tid, int64 tick, int id, intptr_t data)
{
	struct block_list* bl = map->id2bl(id); // TODO: Why does this not use map->bl2md?
	struct mob_data* md = BL_CAST(BL_MOB, bl);

	if( md )
	{
		if( md->spawn_timer != tid )
		{
			ShowError("mob_delayspawn: Timer mismatch: %d != %d\n", tid, md->spawn_timer);
			return 0;
		}
		md->spawn_timer = INVALID_TIMER;
		mob->spawn(md);
	}
	return 0;
}

/*==========================================
 * spawn timing calculation
 *------------------------------------------*/
static int mob_setdelayspawn(struct mob_data *md)
{
	unsigned int spawntime;
	uint32 mode;
	struct mob_db *db;

	nullpo_ret(md);

	if (!md->spawn) //Doesn't has respawn data!
		return unit->free(&md->bl,CLR_DEAD);

	spawntime = md->spawn->delay1; //Base respawn time
	if (md->spawn->delay2) //random variance
		spawntime+= rnd()%md->spawn->delay2;

	//Apply the spawn delay fix [Skotlex]
	db = mob->db(md->spawn->class_);
	mode = db->status.mode;
	if (mode & MD_BOSS) {
		//Bosses
		if (battle_config.boss_spawn_delay != 100) {
			// Divide by 100 first to prevent overflows
			//(precision loss is minimal as duration is in ms already)
			spawntime = spawntime/100*battle_config.boss_spawn_delay;
		}
	} else if (mode&MD_PLANT) {
		//Plants
		if (battle_config.plant_spawn_delay != 100) {
			spawntime = spawntime/100*battle_config.plant_spawn_delay;
		}
	} else if (battle_config.mob_spawn_delay != 100) {
		//Normal mobs
		spawntime = spawntime/100*battle_config.mob_spawn_delay;
	}

	if (spawntime < 5000) //Monsters should never respawn faster than within 5 seconds
		spawntime = 5000;

	if( md->spawn_timer != INVALID_TIMER )
		timer->delete(md->spawn_timer, mob->delayspawn);
	md->spawn_timer = timer->add(timer->gettick()+spawntime, mob->delayspawn, md->bl.id, 0);
	return 0;
}

static int mob_count_sub(struct block_list *bl, va_list ap)
{
	int mobid[10] = { 0 }, i;
	ARR_FIND(0, 10, i, (mobid[i] = va_arg(ap, int)) == 0); //fetch till 0
	if (mobid[0]) { //if there one let's check it otherwise go backward
		struct mob_data *md = BL_CAST(BL_MOB, bl);
		nullpo_ret(md);
		ARR_FIND(0, 10, i, md->class_ == mobid[i]);
		return (i < 10) ? 1 : 0;
	}
	return 1; //backward compatibility
}

/*==========================================
 * Mob spawning. Initialization is also variously here.
 *------------------------------------------*/
static int mob_spawn(struct mob_data *md)
{
	int i=0;
	int64 tick = timer->gettick();
	int64 c = 0;

	nullpo_retr(1, md);
	md->last_thinktime = tick;
	if (md->bl.prev != NULL)
		unit->remove_map(&md->bl,CLR_RESPAWN,ALC_MARK);
	else if (md->spawn && md->class_ != md->spawn->class_) {
		md->class_ = md->spawn->class_;
		status->set_viewdata(&md->bl, md->class_);
		md->db = mob->db(md->class_);
		memcpy(md->name,md->spawn->name,NAME_LENGTH);
	}

	if (md->spawn) { //Respawn data
		md->bl.m = md->spawn->m;
		md->bl.x = md->spawn->x;
		md->bl.y = md->spawn->y;

		if( (md->bl.x == 0 && md->bl.y == 0) || md->spawn->xs || md->spawn->ys ) {
			//Monster can be spawned on an area.
			int sfc_flag = (battle_config.no_spawn_on_player != 0) ? SFC_AVOIDPLAYER : SFC_DEFAULT;
			if (map->search_free_cell(&md->bl, -1, &md->bl.x, &md->bl.y, md->spawn->xs, md->spawn->ys, sfc_flag) != 0) {
				// retry again later
				if( md->spawn_timer != INVALID_TIMER )
					timer->delete(md->spawn_timer, mob->delayspawn);
				md->spawn_timer = timer->add(tick+5000,mob->delayspawn,md->bl.id,0);
				return 1;
			}
		} else if( battle_config.no_spawn_on_player > 99 && map->foreachinrange(mob->count_sub, &md->bl, AREA_SIZE, BL_PC) ) {
			// retry again later (players on sight)
			if( md->spawn_timer != INVALID_TIMER )
				timer->delete(md->spawn_timer, mob->delayspawn);
			md->spawn_timer = timer->add(tick+5000,mob->delayspawn,md->bl.id,0);
			return 1;
		}
	}

	memset(&md->state, 0, sizeof(md->state));
	status_calc_mob(md, SCO_FIRST);
	md->attacked_id = 0;
	md->target_id = 0;
	md->move_fail_count = 0;
	md->ud.state.attack_continue = 0;
	md->ud.target_to = 0;
	md->ud.dir = 0;
	if( md->spawn_timer != INVALID_TIMER )
	{
		timer->delete(md->spawn_timer, mob->delayspawn);
		md->spawn_timer = INVALID_TIMER;
	}

	//md->master_id = 0;
	md->master_dist = 0;

	md->state.aggressive = (md->status.mode&MD_ANGRY) ? 1 : 0;
	md->state.skillstate = MSS_IDLE;
	md->next_walktime = tick+rnd()%1000+MIN_RANDOMWALKTIME;
	md->last_linktime = tick;
	md->dmgtick = tick - 5000;
	md->last_pcneartime = 0;

	for (i = 0, c = tick-MOB_MAX_DELAY; i < MAX_MOBSKILL; i++)
		md->skilldelay[i] = c;

	memset(md->dmglog, 0, sizeof(md->dmglog));
	md->tdmg = 0;

	if (md->lootitem)
		memset(md->lootitem, 0, sizeof(*md->lootitem));

	md->lootitem_count = 0;
	md->dmg_taken_rate = md->db->dmg_taken_rate;

	if(md->db->option)
		// Added for carts, falcons and pecos for cloned monsters. [Valaris]
		md->sc.option = md->db->option;

	// MvP tomb [GreenBox]
	if ( md->tomb_nid )
		mob->mvptomb_destroy(md);

	map->addblock(&md->bl);
	if( map->list[md->bl.m].users )
		clif->spawn(&md->bl);
	skill->unit_move(&md->bl,tick,1);
	mob->use_skill(md, tick, MSC_SPAWN);
	return 0;
}

/*==========================================
 * Determines if the mob can change target. [Skotlex]
 *------------------------------------------*/
static int mob_can_changetarget(const struct mob_data *md, const struct block_list *target, uint32 mode)
{
	nullpo_ret(md);
	nullpo_ret(target);
	// if the monster was provoked ignore the above rule [celest]
	if(md->state.provoke_flag)
	{
		if (md->state.provoke_flag == target->id)
			return 1;
		else if (!(battle_config.mob_ai&0x4))
			return 0;
	}

	switch (md->state.skillstate) {
		case MSS_BERSERK:
			if (!(mode&MD_CHANGETARGET_MELEE))
				return 0;
			return (battle_config.mob_ai&0x4 || check_distance_bl(&md->bl, target, 3));
		case MSS_RUSH:
			return (mode&MD_CHANGETARGET_CHASE) ? 1 : 0;
		case MSS_FOLLOW:
		case MSS_ANGRY:
		case MSS_IDLE:
		case MSS_WALK:
		case MSS_LOOT:
			return 1;
		case MSS_ANY:
		case MSS_ANYTARGET:
		case MSS_DEAD:
		default:
			return 0;
	}
}

/*==========================================
 * Determination for an attack of a monster
 *------------------------------------------*/
static int mob_target(struct mob_data *md, struct block_list *bl, int dist)
{
	nullpo_ret(md);
	nullpo_ret(bl);

	// Nothing will be carried out if there is no mind of changing TAGE by TAGE ending.
	if(md->target_id && !mob->can_changetarget(md, bl, status_get_mode(&md->bl)))
		return 0;

	if(!status->check_skilluse(&md->bl, bl, 0, 0))
		return 0;

	md->target_id = bl->id; // Since there was no disturbance, it locks on to target.
	if (md->state.provoke_flag && bl->id != md->state.provoke_flag)
		md->state.provoke_flag = 0;
	md->min_chase=dist+md->db->range3;
	if(md->min_chase>MAX_MINCHASE)
		md->min_chase=MAX_MINCHASE;
	return 0;
}

/*==========================================
 * The ?? routine of an active monster
 *------------------------------------------*/
static int mob_ai_sub_hard_activesearch(struct block_list *bl, va_list ap)
{
	struct mob_data *md;
	struct block_list **target;
	uint32 mode;
	int dist;

	nullpo_ret(bl);
	md=va_arg(ap,struct mob_data *);
	target= va_arg(ap,struct block_list**);
	mode = va_arg(ap, uint32);
	nullpo_ret(md);
	nullpo_ret(target);

	//If can't seek yet, not an enemy, or you can't attack it, skip.
	if (md->bl.id == bl->id || (*target) == bl || !status->check_skilluse(&md->bl, bl, 0, 0))
		return 0;

	if ((mode&MD_TARGETWEAK) && status->get_lv(bl) >= md->level-5)
		return 0;

	if(battle->check_target(&md->bl,bl,BCT_ENEMY)<=0)
		return 0;

	switch (bl->type) {
		case BL_PC:
			if (BL_UCCAST(BL_PC, bl)->state.gangsterparadise && !(status_get_mode(&md->bl)&MD_BOSS))
				return 0; //Gangster paradise protection.
			FALLTHROUGH
		case BL_NUL:
		case BL_MOB:
		case BL_PET:
		case BL_HOM:
		case BL_MER:
		case BL_ITEM:
		case BL_SKILL:
		case BL_NPC:
		case BL_CHAT:
		case BL_ELEM:
		case BL_ALL:
		default:
			if (battle_config.hom_setting&0x4 &&
				(*target) && (*target)->type == BL_HOM && bl->type != BL_HOM)
				return 0; //For some reason Homun targets are never overridden.

			dist = distance_bl(&md->bl, bl);
			if(
				((*target) == NULL || !check_distance_bl(&md->bl, *target, dist)) &&
				battle->check_range(&md->bl,bl,md->db->range2)
			) { //Pick closest target?
#ifdef ACTIVEPATHSEARCH
				struct walkpath_data wpd;
				bool is_standing = (md->ud.walktimer == INVALID_TIMER);
#ifdef CELL_NOSTACK
				// Do not count target's cell
				short x, y;
				if ((unit->can_reach_bl(&md->bl, bl, distance_bl(&md->bl, bl) + 1, 1, &x, &y)
					&& !path->search(&wpd, &md->bl, md->bl.m, md->bl.x, md->bl.y, x, y, 0, CELL_CHKNOPASS)) // Count walk path cells
#else
				if (!path->search(&wpd, &md->bl, md->bl.m, md->bl.x, md->bl.y, bl->x, bl->y, 0, CELL_CHKNOPASS) // Count walk path cells
#endif
				    || (is_standing && wpd.path_len > md->db->range2) //Standing monsters use range2, walking monsters use range3
				    || (!is_standing && wpd.path_len > md->db->range3)) {
					if (!check_distance_bl(&md->bl, bl, md->status.rhw.range)
					    || !path->search_long(NULL, &md->bl, md->bl.m, md->bl.x, md->bl.y, bl->x, bl->y, CELL_CHKWALL))
						return 0;
				}
#endif
				(*target) = bl;
				md->target_id=bl->id;
				md->min_chase= dist + md->db->range3;
				if(md->min_chase>MAX_MINCHASE)
					md->min_chase=MAX_MINCHASE;
				return 1;
			}
			break;
	}
	return 0;
}

/*==========================================
 * chase target-change routine.
 *------------------------------------------*/
static int mob_ai_sub_hard_changechase(struct block_list *bl, va_list ap)
{
	struct mob_data *md;
	struct block_list **target;

	nullpo_ret(bl);
	md = va_arg(ap,struct mob_data *);
	target = va_arg(ap,struct block_list**);
	nullpo_ret(md);
	nullpo_ret(target);

	//If can't seek yet, not an enemy, or you can't attack it, skip.
	if( md->bl.id == bl->id || *target == bl
	 || battle->check_target(&md->bl,bl,BCT_ENEMY) <= 0
	 || !status->check_skilluse(&md->bl, bl, 0, 0)
	)
		return 0;

	if(battle->check_range (&md->bl, bl, md->status.rhw.range)) {
		(*target) = bl;
		md->target_id=bl->id;
		md->min_chase= md->db->range3;
	}
	return 1;
}

/*==========================================
 * finds nearby bg ally for guardians looking for users to follow.
 *------------------------------------------*/
static int mob_ai_sub_hard_bg_ally(struct block_list *bl, va_list ap)
{
	struct mob_data *md;
	struct block_list **target;

	nullpo_ret(bl);
	md=va_arg(ap,struct mob_data *);
	target= va_arg(ap,struct block_list**);
	nullpo_retr(1, md);
	nullpo_retr(1, target);

	if( status->check_skilluse(&md->bl, bl, 0, 0) && battle->check_target(&md->bl,bl,BCT_ENEMY)<=0 ) {
		(*target) = bl;
	}
	return 1;
}

/*==========================================
 * loot monster item search
 *------------------------------------------*/
static int mob_ai_sub_hard_lootsearch(struct block_list *bl, va_list ap)
{
	struct mob_data* md;
	struct block_list **target;
	int dist;

	md=va_arg(ap,struct mob_data *);
	target= va_arg(ap,struct block_list**);
	nullpo_ret(md);
	nullpo_ret(target);

	dist=distance_bl(&md->bl, bl);
	if(mob->can_reach(md,bl,dist+1, MSS_LOOT) &&
		((*target) == NULL || !check_distance_bl(&md->bl, *target, dist)) //New target closer than previous one.
	) {
		(*target) = bl;
		md->target_id=bl->id;
		md->min_chase=md->db->range3;
	}
	return 0;
}

static int mob_warpchase_sub(struct block_list *bl, va_list ap)
{
	int cur_distance;
	struct block_list *target = va_arg(ap, struct block_list *);
	struct npc_data **target_nd = va_arg(ap, struct npc_data **);
	int *min_distance = va_arg(ap, int *);
	struct npc_data *nd = NULL;

	nullpo_ret(bl);
	nullpo_ret(target);
	nullpo_ret(target_nd);
	nullpo_ret(min_distance);
	Assert_ret(bl->type == BL_NPC);
	nd = BL_UCAST(BL_NPC, bl);

	if(nd->subtype != WARP)
		return 0; //Not a warp

	if(nd->u.warp.mapindex != map_id2index(target->m))
		return 0; //Does not lead to the same map.

	cur_distance = distance_blxy(target, nd->u.warp.x, nd->u.warp.y);
	if (cur_distance < *min_distance) {
		//Pick warp that leads closest to target.
		*target_nd = nd;
		*min_distance = cur_distance;
		return 1;
	}
	return 0;
}

/**
 * Checks if a monster is currently involved in battle,
 * may it be due to aggression or being attacked.
 * @param bl: monster's bl
 * @return true if in battle, false otherwise
 */
static bool mob_is_in_battle_state(const struct mob_data *md)
{
	nullpo_retr(false, md);
	switch (md->state.skillstate) {
	case MSS_BERSERK:
	case MSS_ANGRY:
	case MSS_RUSH:
	case MSS_FOLLOW:
		return true;
	case MSS_ANY:
	case MSS_IDLE:
	case MSS_WALK:
	case MSS_LOOT:
	case MSS_DEAD:
	case MSS_ANYTARGET:
	default:
		return false;
	}
}

/*==========================================
 * Processing of slave monsters
 *------------------------------------------*/
static int mob_ai_sub_hard_slavemob(struct mob_data *md, int64 tick)
{
	struct block_list *bl;

	nullpo_ret(md);
	bl=map->id2bl(md->master_id);

	if (!bl || status->isdead(bl)) {
		status_kill(&md->bl);
		return 1;
	}
	if (bl->prev == NULL)
		return 0; //Master not on a map? Could be warping, do not process.

	if (status_get_mode(&md->bl)&MD_CANMOVE) {
		//If the mob can move, follow around. [Check by Skotlex]
		int old_dist;

		// Distance with between slave and master is measured.
		old_dist=md->master_dist;
		md->master_dist=distance_bl(&md->bl, bl);

		// Since the master was in near immediately before, teleport is carried out and it pursues.
		if(bl->m != md->bl.m ||
			(old_dist<10 && md->master_dist>18) ||
			md->master_dist > MAX_MINCHASE
		){
			md->master_dist = 0;
			unit->warp(&md->bl,bl->m,bl->x,bl->y,CLR_TELEPORT);
			return 1;
		}

		if(md->target_id) //Slave is busy with a target.
			return 0;

		// Approach master if within view range, chase back to Master's area also if standing on top of the master.
		if( (md->master_dist>MOB_SLAVEDISTANCE || md->master_dist == 0)
		 && unit->can_move(&md->bl)
		) {
			short x = bl->x, y = bl->y;
			mob_stop_attack(md);
			const struct mob_data *m_md = BL_CCAST(BL_MOB, bl); // Can be NULL due to master being BL_PC
			// If master is BL_MOB and in battle, lock & chase to master's target instead, unless configured not to.
			if ((bl->type == BL_PC || battle_config.slave_chase_masters_chasetarget == 0 || (m_md != NULL && !mob->is_in_battle_state(m_md)))
			    && map->search_free_cell(&md->bl, bl->m, &x, &y, MOB_SLAVEDISTANCE, MOB_SLAVEDISTANCE, SFC_XY_CENTER) == 0
			    && unit->walk_toxy(&md->bl, x, y, 0) == 0)
				return 1;
		}
	} else if (bl->m != md->bl.m && map_flag_gvg(md->bl.m)) {
		//Delete the summoned mob if it's in a gvg ground and the master is elsewhere. [Skotlex]
		status_kill(&md->bl);
		return 1;
	}

	//Avoid attempting to lock the master's target too often to avoid unnecessary overload. [Skotlex]
	if (DIFF_TICK(md->last_linktime, tick) < MIN_MOBLINKTIME && !md->target_id) {
		struct unit_data  *ud = unit->bl2ud(bl);
		struct mob_data *m_md = BL_CAST(BL_MOB, bl); // Can be NULL due to master being BL_PC
		nullpo_retr(0, ud);
		md->last_linktime = tick;
		struct block_list *tbl = NULL;

		if (battle_config.slave_chase_masters_chasetarget == 1 && m_md != NULL && m_md->target_id != 0) { // possibly chasing something
			tbl = map->id2bl(m_md->target_id);
		} else if (ud->target != 0 && ud->state.attack_continue != 0) {
			tbl = map->id2bl(ud->target);
		} else if (ud->skilltarget != 0) {
			tbl = map->id2bl(ud->skilltarget);
			//Required check as skilltarget is not always an enemy. [Skotlex]
			if (tbl != NULL && battle->check_target(&md->bl, tbl, BCT_ENEMY) <= 0)
				tbl = NULL;
		}
		if (tbl != NULL && status->check_skilluse(&md->bl, tbl, 0, 0) != 0) {
			md->target_id = tbl->id;
			md->min_chase = md->db->range3 + distance_bl(&md->bl, tbl);
			if (md->min_chase > MAX_MINCHASE)
				md->min_chase = MAX_MINCHASE;
			return 1;
		}
	}
	return 0;
}

/*==========================================
 * A lock of target is stopped and mob moves to a standby state.
 * This also triggers idle skill/movement since the AI can get stuck
 * when trying to pick new targets when the current chosen target is
 * unreachable.
 *------------------------------------------*/
static int mob_unlocktarget(struct mob_data *md, int64 tick)
{
	nullpo_ret(md);

	switch (md->state.skillstate) {
	case MSS_WALK:
		if (md->ud.walktimer != INVALID_TIMER)
			break;
		//Because it is not unset when the mob finishes walking.
		md->state.skillstate = MSS_IDLE;
		FALLTHROUGH
	case MSS_IDLE:
		// Idle skill.
		if ((++md->ud.walk_count % IDLE_SKILL_INTERVAL) == 0 && mob->use_skill(md, tick, -1) == 0)
			break;
		//Random walk.
		if (!md->master_id &&
			DIFF_TICK(md->next_walktime, tick) <= 0 &&
			!mob->randomwalk(md,tick))
			//Delay next random walk when this one failed.
			md->next_walktime = tick+rnd()%1000;
		break;
	case MSS_ANY:
	case MSS_LOOT:
	case MSS_DEAD:
	case MSS_ANYTARGET:
	case MSS_BERSERK:
	case MSS_ANGRY:
	case MSS_RUSH:
	case MSS_FOLLOW:
	default:
		mob_stop_attack(md);
		mob_stop_walking(md, STOPWALKING_FLAG_FIXPOS); //Stop chasing.
		md->state.skillstate = MSS_IDLE;
		if(battle_config.mob_ai&0x8) //Walk instantly after dropping target
			md->next_walktime = tick+rnd()%1000;
		else
			md->next_walktime = tick+rnd()%1000+MIN_RANDOMWALKTIME;
		break;
	}
	if (md->target_id) {
		md->target_id=0;
		md->ud.target_to = 0;
		unit->set_target(&md->ud, 0);
	}
	if(battle_config.official_cell_stack_limit && map->count_oncell(md->bl.m, md->bl.x, md->bl.y, BL_CHAR|BL_NPC, 0x1 | 0x2) > battle_config.official_cell_stack_limit) {
		unit->walk_toxy(&md->bl, md->bl.x, md->bl.y, 8);
	}

	return 0;
}
/*==========================================
 * Random walk
 *------------------------------------------*/
static int mob_randomwalk(struct mob_data *md, int64 tick)
{
	const int retrycount=20;
	int i,c,d;
	int speed;

	nullpo_ret(md);

	if(DIFF_TICK(md->next_walktime,tick)>0 ||
	   !unit->can_move(&md->bl) ||
	   !(status_get_mode(&md->bl)&MD_CANMOVE))
		return 0;

	d =12-md->move_fail_count;
	if(d<5) d=5;
	if(d>7) d=7;
	for (i = 0; i < retrycount; i++) {
		// Search of a movable place
		int r=rnd();
		int x=r%(d*2+1)-d;
		int y=r/(d*2+1)%(d*2+1)-d;
		x+=md->bl.x;
		y+=md->bl.y;

		if ((x != md->bl.x || y != md->bl.y) && map->getcell(md->bl.m, &md->bl, x, y, CELL_CHKPASS) != 0
		    && unit->walk_toxy(&md->bl, x, y, 8) == 0)
			break;
	}
	if(i==retrycount){
		md->move_fail_count++;
		if(md->move_fail_count>1000){
			ShowWarning("MOB can't move. random spawn %d, class = %d, at %s (%d,%d)\n",md->bl.id,md->class_,map->list[md->bl.m].name, md->bl.x, md->bl.y);
			md->move_fail_count=0;
			mob->spawn(md);
		}
		return 0;
	}
	speed=status->get_speed(&md->bl);
	for(i=c=0;i<md->ud.walkpath.path_len;i++) {
		// The next walk start time is calculated.
		if(md->ud.walkpath.path[i]&1)
			c+=speed*MOVE_DIAGONAL_COST/MOVE_COST;
		else
			c+=speed;
	}
	md->state.skillstate=MSS_WALK;
	md->move_fail_count=0;
	md->next_walktime = tick+rnd()%1000+MIN_RANDOMWALKTIME+c;
	return 1;
}

static int mob_warpchase(struct mob_data *md, struct block_list *target)
{
	struct npc_data *warp = NULL;
	int distance = AREA_SIZE;
	nullpo_ret(md);
	if (!(target && battle_config.mob_ai&0x40 && battle_config.mob_warp&1))
		return 0; //Can't warp chase.

	if (target->m == md->bl.m && check_distance_bl(&md->bl, target, AREA_SIZE))
		return 0; //No need to do a warp chase.

	if (md->ud.walktimer != INVALID_TIMER &&
		map->getcell(md->bl.m, &md->bl, md->ud.to_x, md->ud.to_y, CELL_CHKNPC))
		return 1; //Already walking to a warp.

	//Search for warps within mob's viewing range.
	map->foreachinrange(mob->warpchase_sub, &md->bl,
	                    md->db->range2, BL_NPC, target, &warp, &distance);

	if (warp != NULL && unit->walk_tobl(&md->bl, &warp->bl, 1, 1) == 0)
		return 1;
	return 0;
}

/*==========================================
 * AI of MOB whose is near a Player
 *------------------------------------------*/
static bool mob_ai_sub_hard(struct mob_data *md, int64 tick)
{
	struct block_list *tbl = NULL, *abl = NULL;
	uint32 mode;
	int view_range, can_move;

	nullpo_retr(false, md);
	if(md->bl.prev == NULL || md->status.hp <= 0)
		return false;

	if (DIFF_TICK(tick, md->last_thinktime) < MIN_MOBTHINKTIME)
		return false;

	md->last_thinktime = tick;

	if (md->ud.skilltimer != INVALID_TIMER)
		return false;

	// Abnormalities
	if(( md->sc.opt1 > 0 && md->sc.opt1 != OPT1_STONEWAIT && md->sc.opt1 != OPT1_BURNING && md->sc.opt1 != OPT1_CRYSTALIZE )
	  || md->sc.data[SC_DEEP_SLEEP] || md->sc.data[SC_BLADESTOP] || md->sc.data[SC__MANHOLE] || md->sc.data[SC_CURSEDCIRCLE_TARGET]) {
		//Should reset targets.
		md->target_id = md->attacked_id = 0;
		return false;
	}

	if (md->sc.count && md->sc.data[SC_BLIND])
		view_range = 3;
	else
		view_range = md->db->range2;
	mode = status_get_mode(&md->bl);

	can_move = (mode&MD_CANMOVE)&&unit->can_move(&md->bl);

	if (md->target_id) {
		//Check validity of current target. [Skotlex]
		struct map_session_data *tsd = NULL;
		tbl = map->id2bl(md->target_id);
		tsd = BL_CAST(BL_PC, tbl);
		if (tbl == NULL || tbl->m != md->bl.m
		 || (md->ud.attacktimer == INVALID_TIMER && !status->check_skilluse(&md->bl, tbl, 0, 0))
		 || (md->ud.walktimer != INVALID_TIMER && !(battle_config.mob_ai&0x1) && !check_distance_bl(&md->bl, tbl, md->min_chase))
		 || (tsd != NULL && ((tsd->state.gangsterparadise && !(mode&MD_BOSS)) || tsd->invincible_timer != INVALID_TIMER))
		) {
			//No valid target
			if (mob->warpchase(md, tbl))
				return true; //Chasing this target.
			if(md->ud.walktimer != INVALID_TIMER && (!can_move || md->ud.walkpath.path_pos <= battle_config.mob_chase_refresh)
				&& (tbl || md->ud.walkpath.path_pos == 0))
				return true; //Walk at least "mob_chase_refresh" cells before dropping the target unless target is non-existent
			mob->unlocktarget(md, tick); //Unlock target
			tbl = NULL;
		}
	}

	// Check for target change.
	if (md->attacked_id && mode&MD_CANATTACK) {
		if (md->attacked_id == md->target_id) {
			//Rude attacked check.
			if (!battle->check_range(&md->bl, tbl, md->status.rhw.range)
			 && ( //Can't attack back and can't reach back.
			       (!can_move && DIFF_TICK(tick, md->ud.canmove_tick) > 0 && (battle_config.mob_ai&0x2 || (md->sc.data[SC_SPIDERWEB] && md->sc.data[SC_SPIDERWEB]->val1)
			      || md->sc.data[SC_WUGBITE] || md->sc.data[SC_VACUUM_EXTREME] || md->sc.data[SC_THORNS_TRAP]
			      || md->sc.data[SC__MANHOLE] // Not yet confirmed if boss will teleport once it can't reach target.
			      || md->walktoxy_fail_count > 0)
			       )
			    || !mob->can_reach(md, tbl, md->min_chase, MSS_RUSH)
			    )
			 && md->state.attacked_count++ >= RUDE_ATTACKED_COUNT
			 && mob->use_skill(md, tick, MSC_RUDEATTACKED) != 0 // If can't rude Attack
			 && can_move != 0 && unit->attempt_escape(&md->bl, tbl, rnd() % 10 + 1) == 0 // Attempt escape
			) {
				//Escaped
				md->attacked_id = 0;
				return true;
			}
		}
		else
		if( (abl = map->id2bl(md->attacked_id)) && (!tbl || mob->can_changetarget(md, abl, mode) || (md->sc.count && md->sc.data[SC__CHAOS]))) {
			int dist;
			if( md->bl.m != abl->m || abl->prev == NULL
			 || (dist = distance_bl(&md->bl, abl)) >= MAX_MINCHASE // Attacker longer than visual area
			 || battle->check_target(&md->bl, abl, BCT_ENEMY) <= 0 // Attacker is not enemy of mob
			 || (battle_config.mob_ai&0x2 && !status->check_skilluse(&md->bl, abl, 0, 0)) // Cannot normal attack back to Attacker
			 || (!battle->check_range(&md->bl, abl, md->status.rhw.range) // Not on Melee Range and ...
			    && ( // Reach check
					(!can_move && DIFF_TICK(tick, md->ud.canmove_tick) > 0 && (battle_config.mob_ai&0x2 || (md->sc.data[SC_SPIDERWEB] && md->sc.data[SC_SPIDERWEB]->val1)
						|| md->sc.data[SC_WUGBITE] || md->sc.data[SC_VACUUM_EXTREME] || md->sc.data[SC_THORNS_TRAP]
						|| md->sc.data[SC__MANHOLE] // Not yet confirmed if boss will teleport once it can't reach target.
						|| md->walktoxy_fail_count > 0)
					)
					   || !mob->can_reach(md, abl, dist+md->db->range3, MSS_RUSH)
			       )
			    )
			) {
				// Rude attacked
				if (md->state.attacked_count++ >= RUDE_ATTACKED_COUNT
				 && mob->use_skill(md, tick, MSC_RUDEATTACKED) != 0 && can_move != 0
				 && tbl == NULL && unit->attempt_escape(&md->bl, abl, rnd() % 10 + 1) == 0
				) {
					//Escaped.
					//TODO: Maybe it shouldn't attempt to run if it has another, valid target?
					md->attacked_id = 0;
					return true;
				}
			}
			else
			if (!(battle_config.mob_ai&0x2) && !status->check_skilluse(&md->bl, abl, 0, 0)) {
				//Can't attack back, but didn't invoke a rude attacked skill...
			} else {
				//Attackable
				if (!tbl || dist < md->status.rhw.range
				 || !check_distance_bl(&md->bl, tbl, dist)
				 || battle->get_target(tbl) != md->bl.id
				) {
					//Change if the new target is closer than the actual one
					//or if the previous target is not attacking the mob. [Skotlex]
					md->target_id = md->attacked_id; // set target
					if (md->state.attacked_count)
					  md->state.attacked_count--; //Should we reset rude attack count?
					md->min_chase = dist+md->db->range3;
					if(md->min_chase>MAX_MINCHASE)
						md->min_chase=MAX_MINCHASE;
					tbl = abl; //Set the new target
				}
			}
		}

		//Clear it since it's been checked for already.
		md->attacked_id = 0;
	}

	// Processing of slave monster
	if (md->master_id > 0 && mob->ai_sub_hard_slavemob(md, tick))
		return true;

	// Scan area for targets
	if (battle_config.monster_loot_type != 1 && tbl == NULL && (mode & MD_LOOTER) != 0x0 && md->lootitem != NULL
	    && DIFF_TICK(tick, md->ud.canact_tick) > 0 && md->lootitem_count < LOOTITEM_SIZE) {
		// Scan area for items to loot, avoid trying to loot if the mob is full and can't consume the items.
		map->foreachinrange (mob->ai_sub_hard_lootsearch, &md->bl, view_range, BL_ITEM, md, &tbl);
	}

	if ((!tbl && mode&MD_AGGRESSIVE) || md->state.skillstate == MSS_FOLLOW) {
		map->foreachinrange(mob->ai_sub_hard_activesearch, &md->bl, view_range, DEFAULT_ENEMY_TYPE(md), md, &tbl, mode);
	} else if ((mode&MD_CHANGECHASE && (md->state.skillstate == MSS_RUSH || md->state.skillstate == MSS_FOLLOW)) || (md->sc.count && md->sc.data[SC__CHAOS])) {
		int search_size;
		search_size = view_range<md->status.rhw.range ? view_range:md->status.rhw.range;
		map->foreachinrange (mob->ai_sub_hard_changechase, &md->bl, search_size, DEFAULT_ENEMY_TYPE(md), md, &tbl);
	}

	if (!tbl) { //No targets available.
		if (mode&MD_ANGRY && !md->state.aggressive)
			md->state.aggressive = 1; //Restore angry state when no targets are available.

		/* bg guardians follow allies when no targets nearby */
		if( md->bg_id && mode&MD_CANATTACK ) {
			if( md->ud.walktimer != INVALID_TIMER )
				return true;/* we are already moving */
			map->foreachinrange (mob->ai_sub_hard_bg_ally, &md->bl, view_range, BL_PC, md, &tbl, mode);
			if( tbl ) {
				if (distance_blxy(&md->bl, tbl->x, tbl->y) <= 3 || unit->walk_tobl(&md->bl, tbl, 1, 1) == 0)
					return true;/* we're moving or close enough don't unlock the target. */
			}
		}

		//This handles triggering idle/walk skill.
		mob->unlocktarget(md, tick);
		return true;
	}

	//Target exists, attack or loot as applicable.
	if (tbl->type == BL_ITEM) {
		//Loot time.
		struct flooritem_data *fitem = BL_UCAST(BL_ITEM, tbl);
		if (md->ud.target == tbl->id && md->ud.walktimer != INVALID_TIMER)
			return true; //Already locked.
		if (md->lootitem == NULL) {
			//Can't loot...
			mob->unlocktarget (md, tick);
			return true;
		}
		if (!check_distance_bl(&md->bl, tbl, 1)) {
			//Still not within loot range.
			if (!(mode&MD_CANMOVE)) {
				//A looter that can't move? Real smart.
				mob->unlocktarget(md,tick);
				return true;
			}
			if (!can_move) //Stuck. Wait before walking.
				return true;
			md->state.skillstate = MSS_LOOT;
			if (unit->walk_tobl(&md->bl, tbl, 1, 1) != 0)
				mob->unlocktarget(md, tick); //Can't loot...
			return true;
		}
		//Within looting range.
		if (md->ud.attacktimer != INVALID_TIMER)
			return true; //Busy attacking?

		//Logs items, taken by (L)ooter Mobs [Lupus]
		logs->pick_mob(md, LOG_TYPE_LOOT, fitem->item_data.amount, &fitem->item_data, NULL);

		if (md->lootitem_count < LOOTITEM_SIZE) {
			memcpy (&md->lootitem[md->lootitem_count++], &fitem->item_data, sizeof(md->lootitem[0]));
		} else {
			//Destroy first looted item...
			if (md->lootitem[0].card[0] == CARD0_PET)
				intif->delete_petdata( MakeDWord(md->lootitem[0].card[1],md->lootitem[0].card[2]) );
			memmove(&md->lootitem[0], &md->lootitem[1], (LOOTITEM_SIZE-1)*sizeof(md->lootitem[0]));
			memcpy (&md->lootitem[LOOTITEM_SIZE-1], &fitem->item_data, sizeof(md->lootitem[0]));
		}
		if (pc->db_checkid(md->vd->class)) {
			//Give them walk act/delay to properly mimic players. [Skotlex]
			clif->takeitem(&md->bl,tbl);
			md->ud.canact_tick = tick + md->status.amotion;
			unit->set_walkdelay(&md->bl, tick, md->status.amotion, 1);
		}
		//Clear item.
		map->clearflooritem (tbl);
		mob->unlocktarget (md,tick);
		return true;
	}

	//Attempt to attack.
	//At this point we know the target is attackable, we just gotta check if the range matches.
	if (battle->check_range(&md->bl, tbl, md->status.rhw.range) && !(md->sc.option&OPTION_HIDE)) {
		//Target within range and able to use normal attack, engage
		if (md->ud.target != tbl->id || md->ud.attacktimer == INVALID_TIMER)
		{ //Only attack if no more attack delay left
			if(tbl->type == BL_PC)
				mob->log_damage(md, tbl, 0); //Log interaction (counts as 'attacker' for the exp bonus)
			unit->attack(&md->bl,tbl->id,1);
		}
		return true;
	}

	//Monsters in berserk state, unable to use normal attacks, will always attempt a skill
	if(md->ud.walktimer == INVALID_TIMER && (md->state.skillstate == MSS_BERSERK || md->state.skillstate == MSS_ANGRY)) {
		if (DIFF_TICK(md->ud.canmove_tick, tick) <= MIN_MOBTHINKTIME && DIFF_TICK(md->ud.canact_tick, tick) < -MIN_MOBTHINKTIME*IDLE_SKILL_INTERVAL)
		{ //Only use skill if able to walk on next tick and not used a skill the last second
			mob->use_skill(md, tick, -1);
		}
	}

	//Target still in attack range, no need to chase the target
	if(battle->check_range(&md->bl, tbl, md->status.rhw.range))
		return true;

	//Only update target cell / drop target after having moved at least "mob_chase_refresh" cells
	if(md->ud.walktimer != INVALID_TIMER && (!can_move || md->ud.walkpath.path_pos <= battle_config.mob_chase_refresh))
		return true;

	//Out of range...
	if (!(mode&MD_CANMOVE) || (!can_move && DIFF_TICK(tick, md->ud.canmove_tick) > 0)) {
		//Can't chase. Immobile and trapped mobs should unlock target and use an idle skill.
		if (md->ud.attacktimer == INVALID_TIMER)
		{ //Only unlock target if no more attack delay left
			//This handles triggering idle/walk skill.
			mob->unlocktarget(md,tick);
		}
		return true;
	}

	if (md->ud.walktimer != INVALID_TIMER && md->ud.target == tbl->id &&
		(
			!(battle_config.mob_ai&0x1) ||
			check_distance_blxy(tbl, md->ud.to_x, md->ud.to_y, md->status.rhw.range)
	)) //Current target tile is still within attack range.
		return true;

	//Follow up if possible.
	//Hint: Chase skills are handled in the walktobl routine
	if (mob->can_reach(md, tbl, md->min_chase, MSS_RUSH) == 0
	    || unit->walk_tobl(&md->bl, tbl, md->status.rhw.range, 2) != 0)
		mob->unlocktarget(md,tick);

	return true;
}

static int mob_ai_sub_hard_timer(struct block_list *bl, va_list ap)
{
	struct mob_data *md = NULL;
	int64 tick = va_arg(ap, int64);

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_MOB);
	md = BL_UCAST(BL_MOB, bl);

	if (mob->ai_sub_hard(md, tick)) {
		//Hard AI triggered.
		if(!md->state.spotted)
			md->state.spotted = 1;
		md->last_pcneartime = tick;
	}
	return 0;
}

/*==========================================
 * Serious processing for mob in PC field of view (foreachclient)
 *------------------------------------------*/
static int mob_ai_sub_foreachclient(struct map_session_data *sd, va_list ap)
{
	int64 tick;
	nullpo_ret(sd);
	tick=va_arg(ap, int64);
	map->foreachinrange(mob->ai_sub_hard_timer,&sd->bl, AREA_SIZE+ACTIVE_AI_RANGE, BL_MOB,tick);

	return 0;
}

/*==========================================
 * Negligent mode MOB AI (PC is not in near)
 *------------------------------------------*/
static int mob_ai_sub_lazy(struct mob_data *md, va_list args)
{
	int64 tick;

	nullpo_ret(md);

	if(md->bl.prev == NULL)
		return 0;

	tick = va_arg(args, int64);

	if (battle_config.mob_ai&0x20 && map->list[md->bl.m].users>0)
		return (int)mob->ai_sub_hard(md, tick);

	if (md->bl.prev==NULL || md->status.hp == 0)
		return 1;

	if (battle_config.mob_active_time
	 && md->last_pcneartime
	 && !(md->status.mode&MD_BOSS)
	 && DIFF_TICK(tick,md->last_thinktime) > MIN_MOBTHINKTIME
	) {
		if (DIFF_TICK(tick,md->last_pcneartime) < battle_config.mob_active_time)
			return (int)mob->ai_sub_hard(md, tick);
		md->last_pcneartime = 0;
	}

	if(battle_config.boss_active_time &&
		md->last_pcneartime &&
		(md->status.mode&MD_BOSS) &&
		DIFF_TICK(tick,md->last_thinktime) > MIN_MOBTHINKTIME)
	{
		if (DIFF_TICK(tick,md->last_pcneartime) < battle_config.boss_active_time)
			return (int)mob->ai_sub_hard(md, tick);
		md->last_pcneartime = 0;
	}

	if(DIFF_TICK(tick,md->last_thinktime)< 10*MIN_MOBTHINKTIME)
		return 0;

	md->last_thinktime=tick;

	if (md->master_id) {
		mob->ai_sub_hard_slavemob(md,tick);
		return 0;
	}

	if( DIFF_TICK(md->next_walktime,tick) < 0 && (status_get_mode(&md->bl)&MD_CANMOVE) && unit->can_move(&md->bl) ) {
		if( rnd()%1000 < MOB_LAZYMOVEPERC(md) )
			mob->randomwalk(md, tick);
	}
	else if( md->ud.walktimer == INVALID_TIMER )
	{
		//Because it is not unset when the mob finishes walking.
		md->state.skillstate = MSS_IDLE;
		if( rnd()%1000 < MOB_LAZYSKILLPERC(md) ) //Chance to do a mob's idle skill.
			mob->use_skill(md, tick, -1);
	}

	return 0;
}

/*==========================================
 * Negligent processing for mob outside PC field of view   (interval timer function)
 *------------------------------------------*/
static int mob_ai_lazy(int tid, int64 tick, int id, intptr_t data)
{
	map->foreachmob(mob->ai_sub_lazy,tick);
	return 0;
}

/*==========================================
 * Serious processing for mob in PC field of view   (interval timer function)
 *------------------------------------------*/
static int mob_ai_hard(int tid, int64 tick, int id, intptr_t data)
{

	if (battle_config.mob_ai&0x20)
		map->foreachmob(mob->ai_sub_lazy,tick);
	else
		map->foreachpc(mob->ai_sub_foreachclient,tick);

	return 0;
}

/**
 * Adds random options of a given options drop group into item.
 *
 * @param item : item receiving random options
 * @param options : Random Option Drop Group to be used
 */
static void mob_setdropitem_options(struct item *item, struct optdrop_group *options)
{
	nullpo_retv(item);
	nullpo_retv(options);

	for (int i = 0; i < options->optslot_count; i++) {
		if (rnd() % 10000 >= options->optslot_rate[i])
			continue;

		// count avoids a too long loop that would cause lag.
		// if after option_drop_max_loop full iterations (running through all possibilities)
		// it still fails to pick one, it'll stop at one random index in the next iteration
		int count = battle_config.option_drop_max_loop * options->optslot[i].option_count + (rnd() % options->optslot[i].option_count);
		int idx = 0;
		while (count > 0 && rnd() % 10000 >= options->optslot[i].options[idx].rate) {
			idx = (idx + 1) % options->optslot[i].option_count;
			--count;
		}

		item->option[i].index = options->optslot[i].options[idx].id;

		int min = options->optslot[i].options[idx].min;
		int max = options->optslot[i].options[idx].max;
		item->option[i].value = min + (rnd() % (max - min + 1));
	}
}

/*==========================================
 * Initializes the delay drop structure for mob-dropped items.
 *------------------------------------------*/
static struct item_drop *mob_setdropitem(int nameid, struct optdrop_group *options, int qty, struct item_data *data)
{
	struct item_drop *drop = ers_alloc(item_drop_ers, struct item_drop);
	drop->item_data.nameid = nameid;
	drop->item_data.amount = qty;
	drop->item_data.identify = data ? itemdb->isidentified2(data) : itemdb->isidentified(nameid);

	// Set item options [KirieZ]
	if (options != NULL)
		mob->setdropitem_options(&drop->item_data, options);

	drop->showdropeffect = true;
	drop->next = NULL;
	return drop;
}

/*==========================================
 * Initializes the delay drop structure for mob-looted items.
 *------------------------------------------*/
static struct item_drop *mob_setlootitem(struct item *item)
{
	struct item_drop *drop ;

	nullpo_retr(NULL, item);
	drop = ers_alloc(item_drop_ers, struct item_drop);
	memcpy(&drop->item_data, item, sizeof(struct item));
	drop->showdropeffect = false;
	drop->next = NULL;
	return drop;
}

/*==========================================
 * item drop with delay (timer function)
 *------------------------------------------*/
static int mob_delay_item_drop(int tid, int64 tick, int id, intptr_t data)
{
	struct item_drop_list *list;
	struct item_drop *ditem;
	list=(struct item_drop_list *)data;
	ditem = list->item;
	while (ditem) {
		struct item_drop *ditem_prev;
		map->addflooritem(NULL, &ditem->item_data,ditem->item_data.amount,
		    list->m,list->x,list->y,
		    list->first_charid,list->second_charid,list->third_charid,0,
		    ditem->showdropeffect);
		ditem_prev = ditem;
		ditem = ditem->next;
		ers_free(item_drop_ers, ditem_prev);
	}
	ers_free(item_drop_list_ers, list);
	return 0;
}

/*==========================================
 * Sets the item_drop into the item_drop_list.
 * Also performs logging and autoloot if enabled.
 * rate is the drop-rate of the item, required for autoloot.
 * flag : Killed only by homunculus?
 *------------------------------------------*/
static void mob_item_drop(struct mob_data *md, struct item_drop_list *dlist, struct item_drop *ditem, int loot, int drop_rate, unsigned short flag)
{
	struct map_session_data *sd = NULL;

	nullpo_retv(md);
	nullpo_retv(dlist);
	nullpo_retv(ditem);
	//Logs items, dropped by mobs [Lupus]
	logs->pick_mob(md, loot?LOG_TYPE_LOOT:LOG_TYPE_PICKDROP_MONSTER, -ditem->item_data.amount, &ditem->item_data, NULL);

	sd = map->charid2sd(dlist->first_charid);
	if( sd == NULL ) sd = map->charid2sd(dlist->second_charid);
	if( sd == NULL ) sd = map->charid2sd(dlist->third_charid);

	if( sd
		&& (drop_rate <= sd->state.autoloot || pc->isautolooting(sd, ditem->item_data.nameid))
		&& (!map->list[sd->bl.m].flag.noautoloot)
		&& (battle_config.idle_no_autoloot == 0 || DIFF_TICK(sockt->last_tick, sd->idletime) < battle_config.idle_no_autoloot)
		&& (battle_config.homunculus_autoloot?1:!flag)
#ifdef AUTOLOOT_DISTANCE
		&& sd->bl.m == md->bl.m
		&& check_distance_blxy(&sd->bl, dlist->x, dlist->y, AUTOLOOT_DISTANCE)
#endif
	) {
		//Autoloot.
		if (party->share_loot(party->search(sd->status.party_id),
			sd, &ditem->item_data, sd->status.char_id) == 0
		) {
			ers_free(item_drop_ers, ditem);
			return;
		}
	}
	ditem->next = dlist->item;
	dlist->item = ditem;
}

static int mob_timer_delete(int tid, int64 tick, int id, intptr_t data)
{
	struct block_list* bl = map->id2bl(id); // TODO: Why does this not use map->id2md?
	struct mob_data* md = BL_CAST(BL_MOB, bl);

	if( md )
	{
		if( md->deletetimer != tid )
		{
			ShowError("mob_timer_delete: Timer mismatch: %d != %d\n", tid, md->deletetimer);
			return 0;
		}
		//for Alchemist CANNIBALIZE [Lupus]
		md->deletetimer = INVALID_TIMER;
		unit->free(bl, CLR_TELEPORT);
	}
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
static int mob_deleteslave_sub(struct block_list *bl, va_list ap)
{
	struct mob_data *md = NULL;
	int id = va_arg(ap, int);

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_MOB);
	md = BL_UCAST(BL_MOB, bl);

	if(md->master_id > 0 && md->master_id == id )
		status_kill(bl);
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
static int mob_deleteslave(struct mob_data *md)
{
	nullpo_ret(md);

	map->foreachinmap(mob->deleteslave_sub, md->bl.m, BL_MOB,md->bl.id);
	return 0;
}
// Mob respawning through KAIZEL or NPC_REBIRTH [Skotlex]
static int mob_respawn(int tid, int64 tick, int id, intptr_t data)
{
	struct block_list *bl = map->id2bl(id);

	if(!bl) return 0;
	status->revive(bl, (uint8)data, 0);
	return 1;
}

static void mob_log_damage(struct mob_data *md, struct block_list *src, int damage)
{
	int char_id = 0, flag = MDLF_NORMAL;

	nullpo_retv(md);
	nullpo_retv(src);

	if( damage < 0 )
		return; //Do nothing for absorbed damage.
	if( !damage && !(src->type&DEFAULT_ENEMY_TYPE(md)) )
		return; //Do not log non-damaging effects from non-enemies.
	if( src->id == md->bl.id )
		return; //Do not log self-damage.

	switch( src->type )
	{
		case BL_PC:
		{
			const struct map_session_data *sd = BL_UCCAST(BL_PC, src);
			char_id = sd->status.char_id;
			if( damage )
				md->attacked_id = src->id;
			break;
		}
		case BL_HOM:
		{
			const struct homun_data *hd = BL_UCCAST(BL_HOM, src);
			flag = MDLF_HOMUN;
			if( hd->master )
				char_id = hd->master->status.char_id;
			if( damage )
				md->attacked_id = src->id;
			break;
		}
		case BL_MER:
		{
			const struct mercenary_data *mer = BL_UCCAST(BL_MER, src);
			if( mer->master )
				char_id = mer->master->status.char_id;
			if( damage )
				md->attacked_id = src->id;
			break;
		}
		case BL_PET:
		{
			const struct pet_data *pd = BL_UCCAST(BL_PET, src);
			flag = MDLF_PET;
			if( pd->msd )
			{
				char_id = pd->msd->status.char_id;
				if( damage ) //Let mobs retaliate against the pet's master [Skotlex]
					md->attacked_id = pd->msd->bl.id;
			}
			break;
		}
		case BL_MOB:
		{
			const struct mob_data *md2 = BL_UCCAST(BL_MOB, src);
			if (md2->special_state.ai != AI_NONE && md2->master_id) {
				struct map_session_data* msd = map->id2sd(md2->master_id);
				if( msd )
					char_id = msd->status.char_id;
			}
			if( !damage )
				break;
			//Let players decide whether to retaliate versus the master or the mob. [Skotlex]
			if( md2->master_id && battle_config.retaliate_to_master )
				md->attacked_id = md2->master_id;
			else
				md->attacked_id = src->id;
			break;
		}
		case BL_ELEM:
		{
			const struct elemental_data *ele = BL_UCCAST(BL_ELEM, src);
			if( ele->master )
				char_id = ele->master->status.char_id;
			if( damage )
				md->attacked_id = src->id;
			break;
		}
		case BL_NUL:
		case BL_ITEM:
		case BL_SKILL:
		case BL_NPC:
		case BL_CHAT:
		case BL_ALL:
		default: //For all unhandled types.
			md->attacked_id = src->id;
	}

	if( char_id )
	{ //Log damage...
		int i,minpos;
		unsigned int mindmg;
		for(i=0,minpos=DAMAGELOG_SIZE-1,mindmg=UINT_MAX;i<DAMAGELOG_SIZE;i++){
			if(md->dmglog[i].id==char_id &&
				md->dmglog[i].flag==flag)
				break;
			if(md->dmglog[i].id==0) {
				//Store data in first empty slot.
				md->dmglog[i].id  = char_id;
				md->dmglog[i].flag= flag;
				break;
			}
			if (md->dmglog[i].dmg<mindmg && i) {
				//Never overwrite first hit slot (he gets double exp bonus)
				minpos=i;
				mindmg=md->dmglog[i].dmg;
			}
		}
		if(i<DAMAGELOG_SIZE)
			md->dmglog[i].dmg+=damage;
		else {
			md->dmglog[minpos].id  = char_id;
			md->dmglog[minpos].flag= flag;
			md->dmglog[minpos].dmg = damage;
		}
#if (PACKETVER >= 20120404 && PACKETVER < 20131223)
		// Show HP bar to all chars who hit the mob (fixes TF_STEAL not showing HP bar right away but only when target leaves/re-enters sight range)
		if (battle_config.show_monster_hp_bar != 0 && (md->status.mode & MD_BOSS) == 0) {
			struct map_session_data *sd = map->charid2sd(char_id);
			if (sd != NULL && check_distance_bl(&md->bl, &sd->bl, AREA_SIZE)) // check if in range
				clif->monster_hp_bar(md, sd);
		}
#endif
	}
	return;
}
//Call when a mob has received damage.
static void mob_damage(struct mob_data *md, struct block_list *src, int damage)
{
	nullpo_retv(md);
	if (damage > 0) { //Store total damage...
		if (UINT_MAX - (unsigned int)damage > md->tdmg)
			md->tdmg+=damage;
		else if (md->tdmg == UINT_MAX)
			damage = 0; //Stop recording damage once the cap has been reached.
		else { //Cap damage log...
			damage = (int)(UINT_MAX - md->tdmg);
			md->tdmg = UINT_MAX;
		}
		if (md->state.aggressive) { //No longer aggressive, change to retaliate AI.
			md->state.aggressive = 0;
			if(md->state.skillstate== MSS_ANGRY)
				md->state.skillstate = MSS_BERSERK;
			if(md->state.skillstate== MSS_FOLLOW)
				md->state.skillstate = MSS_RUSH;
		}
		//Log damage
		if (src)
			mob->log_damage(md, src, damage);
		md->dmgtick = timer->gettick();

		// Achievements [Smokexyz/Hercules]
		if (src != NULL && src->type == BL_PC)
			achievement->validate_mob_damage(BL_UCAST(BL_PC, src), damage, false);
	}

	if (battle_config.show_mob_info&3)
		clif->blname_ack(0, &md->bl);

#if PACKETVER >= 20131223
	// Resend ZC_NOTIFY_MOVEENTRY to Update the HP
	if (clif->show_monster_hp_bar(&md->bl))
		clif->set_unit_walking(&md->bl, NULL, unit->bl2ud(&md->bl), AREA);
#endif

	if (!src)
		return;

#if (PACKETVER >= 20120404 && PACKETVER < 20131223)
	if (battle_config.show_monster_hp_bar && !(md->status.mode&MD_BOSS)) {
		int i;
		for(i = 0; i < DAMAGELOG_SIZE; i++){ // must show hp bar to all char who already hit the mob.
			if (md->dmglog[i].id) {
				struct map_session_data *sd = map->charid2sd(md->dmglog[i].id);
				if (sd && check_distance_bl(&md->bl, &sd->bl, AREA_SIZE)) // check if in range
					clif->monster_hp_bar(md, sd);
			}
		}
	}
#endif
}

/*==========================================
 * Signals death of mob.
 * type&1 -> no drops, type&2 -> no exp
 *------------------------------------------*/
static int mob_dead(struct mob_data *md, struct block_list *src, int type)
{
	GUARD_MAP_LOCK

	struct status_data *mstatus;
	struct map_session_data *sd = BL_CAST(BL_PC, src);
	struct map_session_data *tmpsd[DAMAGELOG_SIZE] = { NULL };
	struct map_session_data *mvp_sd = sd, *second_sd = NULL, *third_sd = NULL;
	struct item_data *id = NULL;

	struct {
		struct party_data *p;
		int id,zeny;
		unsigned int base_exp,job_exp;
	} pt[DAMAGELOG_SIZE] = { { 0 } };
	int i, temp, count, m;
	int dmgbltypes = 0;  // bitfield of all bl types, that caused damage to the mob and are eligible for exp distribution
	unsigned int mvp_damage;
	int64 tick = timer->gettick();
	bool rebirth, homkillonly;

	nullpo_retr(3, md);
	m = md->bl.m;
	Assert_retr(false, m >= 0 && m < map->count);
	mstatus = &md->status;

	if( md->guardian_data && md->guardian_data->number >= 0 && md->guardian_data->number < MAX_GUARDIANS )
		guild->castledatasave(md->guardian_data->castle->castle_id, 10+md->guardian_data->number,0);

	if( src )
	{ // Use Dead skill only if not killed by Script or Command
		md->state.skillstate = MSS_DEAD;
		mob->use_skill(md, tick, -1);
	}

	map->freeblock_lock();

	if (src != NULL && src->type == BL_MOB)
		mob->unlocktarget(BL_UCAST(BL_MOB, src), tick);

	// filter out entries not eligible for exp distribution
	for(i = 0, count = 0, mvp_damage = 0; i < DAMAGELOG_SIZE && md->dmglog[i].id; i++) {
		struct map_session_data* tsd = map->charid2sd(md->dmglog[i].id);

		if(tsd == NULL)
			continue; // skip empty entries
		if(tsd->bl.m != m)
			continue; // skip players not on this map
		count++; //Only logged into same map chars are counted for the total.
		if (pc_isdead(tsd))
			continue; // skip dead players
		if(md->dmglog[i].flag == MDLF_HOMUN && !homun_alive(tsd->hd))
			continue; // skip homunc's share if inactive
		if( md->dmglog[i].flag == MDLF_PET && (!tsd->status.pet_id || !tsd->pd) )
			continue; // skip pet's share if inactive

		if(md->dmglog[i].dmg > mvp_damage) {
			third_sd = second_sd;
			second_sd = mvp_sd;
			mvp_sd = tsd;
			mvp_damage = md->dmglog[i].dmg;
		}

		tmpsd[i] = tsd; // record as valid damage-log entry

		switch( md->dmglog[i].flag ) {
			case MDLF_NORMAL: dmgbltypes|= BL_PC;  break;
			case MDLF_HOMUN:  dmgbltypes|= BL_HOM; break;
			case MDLF_PET:    dmgbltypes|= BL_PET; break;
		}
	}

	// determines, if the monster was killed by homunculus' damage only
	homkillonly = (bool)( ( dmgbltypes&BL_HOM ) && !( dmgbltypes&~BL_HOM ) );

	if (!battle_config.exp_calc_type && count > 1) {
		//Apply first-attacker 200% exp share bonus
		//TODO: Determine if this should go before calculating the MVP player instead of after.
		if (UINT_MAX - md->dmglog[0].dmg > md->tdmg) {
			md->tdmg += md->dmglog[0].dmg;
			md->dmglog[0].dmg<<=1;
		} else {
			md->dmglog[0].dmg+= UINT_MAX - md->tdmg;
			md->tdmg = UINT_MAX;
		}
	}

	if( !(type&2) //No exp
	 && (!map->list[m].flag.pvp || battle_config.pvp_exp) //Pvp no exp rule [MouseJstr]
	 && (!md->master_id || md->special_state.ai == AI_NONE) //Only player-summoned mobs do not give exp. [Skotlex]
	 && (!map->list[m].flag.nobaseexp || !map->list[m].flag.nojobexp) //Gives Exp
	) { //Experience calculation.
		int bonus = 100; //Bonus on top of your share (common to all attackers).
		int pnum = 0;
		if (md->sc.data[SC_RICHMANKIM])
			bonus += md->sc.data[SC_RICHMANKIM]->val2;
		if(sd) {
			temp = status->get_class(&md->bl);
			if(sd->sc.data[SC_MIRACLE]) i = 2; //All mobs are Star Targets
			else
			ARR_FIND(0, MAX_PC_FEELHATE, i, temp == sd->hate_mob[i] &&
				(battle_config.allow_skill_without_day || pc->sg_info[i].day_func()));
			if(i<MAX_PC_FEELHATE && (temp=pc->checkskill(sd,pc->sg_info[i].bless_id)) > 0)
				bonus += (i==2?20:10)*temp;
		}
		if(battle_config.mobs_level_up && md->level > md->db->lv) // [Valaris]
			bonus += (md->level-md->db->lv)*battle_config.mobs_level_up_exp_rate;

	for(i = 0; i < DAMAGELOG_SIZE && md->dmglog[i].id; i++) {
		int flag=1,zeny=0;
		unsigned int base_exp, job_exp;
		double per; //Your share of the mob's exp

		if (!tmpsd[i]) continue;

		if (!battle_config.exp_calc_type && md->tdmg)
			//jAthena's exp formula based on total damage.
			per = (double)md->dmglog[i].dmg/(double)md->tdmg;
		else {
			//eAthena's exp formula based on max hp.
			per = (double)md->dmglog[i].dmg/(double)mstatus->max_hp;
			if (per > 2) per = 2; // prevents unlimited exp gain
		}

		if (count>1 && battle_config.exp_bonus_attacker) {
			//Exp bonus per additional attacker.
			if (count > battle_config.exp_bonus_max_attacker)
				count = battle_config.exp_bonus_max_attacker;
			per += per*((count-1)*battle_config.exp_bonus_attacker)/100.;
		}

		// change experience for different sized monsters [Valaris]
		if (battle_config.mob_size_influence) {
			switch( md->special_state.size ) {
				case SZ_MEDIUM:
					per /= 2.;
					break;
				case SZ_BIG:
					per *= 2.;
					break;
			}
		}

		if( md->dmglog[i].flag == MDLF_PET )
			per *= battle_config.pet_attack_exp_rate/100.;

		if(battle_config.zeny_from_mobs && md->level) {
			 // zeny calculation moblv + random moblv [Valaris]
			zeny=(int) ((md->level+rnd()%md->level)*per*bonus/100.);
			if(md->db->mexp > 0)
				zeny*=rnd()%250;
		}

		if (map->list[m].flag.nobaseexp || !md->db->base_exp)
			base_exp = 0;
		else
			base_exp = (unsigned int)cap_value(md->db->base_exp * per * bonus/100. * map->list[m].bexp/100., 1, UINT_MAX);

		if (map->list[m].flag.nojobexp || !md->db->job_exp || md->dmglog[i].flag == MDLF_HOMUN) //Homun earned job-exp is always lost.
			job_exp = 0;
		else
			job_exp = (unsigned int)cap_value(md->db->job_exp * per * bonus/100. * map->list[m].jexp/100., 1, UINT_MAX);

		if ( (temp = tmpsd[i]->status.party_id) > 0 ) {
			int j;
			for( j = 0; j < pnum && pt[j].id != temp; j++ ); //Locate party.

			if( j == pnum ){ //Possibly add party.
				pt[pnum].p = party->search(temp);
				if(pt[pnum].p && pt[pnum].p->party.exp) {
					pt[pnum].id = temp;
					pt[pnum].base_exp = base_exp;
					pt[pnum].job_exp = job_exp;
					pt[pnum].zeny = zeny; // zeny share [Valaris]
					pnum++;
					flag=0;
				}
			} else {
				//Add to total
				if (pt[j].base_exp > UINT_MAX - base_exp)
					pt[j].base_exp = UINT_MAX;
				else
					pt[j].base_exp += base_exp;

				if (pt[j].job_exp > UINT_MAX - job_exp)
					pt[j].job_exp = UINT_MAX;
				else
					pt[j].job_exp += job_exp;

				pt[j].zeny+=zeny;  // zeny share [Valaris]
				flag=0;
			}
		}
		if(base_exp && md->dmglog[i].flag == MDLF_HOMUN) //tmpsd[i] is null if it has no homunc.
			homun->gainexp(tmpsd[i]->hd, base_exp);
		if(flag) {
			if(base_exp || job_exp) {
				if( md->dmglog[i].flag != MDLF_PET || battle_config.pet_attack_exp_to_master ) {
					pc->gainexp(tmpsd[i], &md->bl, base_exp, job_exp, false);
				}
			}
			if(zeny) // zeny from mobs [Valaris]
				pc->getzeny(tmpsd[i], zeny, LOG_TYPE_PICKDROP_MONSTER, NULL);

			if (!md->special_state.clone && !mob->is_clone(md->class_))
				achievement->validate_mob_kill(tmpsd[i], md->db->mob_id); // Achievements [Smokexyz/Hercules]
		}
	}

	for( i = 0; i < pnum; i++ ) //Party share.
		party->exp_share(pt[i].p, &md->bl, pt[i].base_exp,pt[i].job_exp,pt[i].zeny);

	} //End EXP giving.

	if( !(type&1) && !map->list[m].flag.nomobloot && !md->state.rebirth && (
		md->special_state.ai == AI_NONE || //Non special mob
		battle_config.alchemist_summon_reward == 2 || //All summoned give drops
		(md->special_state.ai == AI_SPHERE && battle_config.alchemist_summon_reward == 1) //Marine Sphere Drops items.
		) )
	{ // Item Drop
		struct item_drop_list *dlist = ers_alloc(item_drop_list_ers, struct item_drop_list);
		struct item_drop *ditem;
		struct item_data* it = NULL;
		int drop_rate;
#ifdef RENEWAL_DROP
		int drop_modifier = mvp_sd    ? pc->level_penalty_mod( md->level - mvp_sd->status.base_level, md->status.race, md->status.mode, 2)   :
							second_sd ? pc->level_penalty_mod( md->level - second_sd->status.base_level, md->status.race, md->status.mode, 2):
							third_sd  ? pc->level_penalty_mod( md->level - third_sd->status.base_level, md->status.race, md->status.mode, 2) :
							100;/* no player was attached, we don't use any modifier (100 = rates are not touched) */
#endif
		dlist->m = md->bl.m;
		dlist->x = md->bl.x;
		dlist->y = md->bl.y;
		dlist->first_charid = (mvp_sd ? mvp_sd->status.char_id : 0);
		dlist->second_charid = (second_sd ? second_sd->status.char_id : 0);
		dlist->third_charid = (third_sd ? third_sd->status.char_id : 0);
		dlist->item = NULL;

		for (i = 0; i < MAX_MOB_DROP; i++)
		{
			if (md->db->dropitem[i].nameid <= 0)
				continue;
			if ( !(it = itemdb->exists(md->db->dropitem[i].nameid)) )
				continue;
			drop_rate = md->db->dropitem[i].p;

			// change drops depending on monsters size [Valaris]
			if (battle_config.mob_size_influence) {
				if (md->special_state.size == SZ_MEDIUM && drop_rate >= 2)
					drop_rate /= 2;
				else if( md->special_state.size == SZ_BIG)
					drop_rate *= 2;
			}

			if (src != NULL) {
				//Drops affected by luk as a fixed increase [Valaris]
				if (battle_config.drops_by_luk)
					drop_rate += status_get_luk(src) * battle_config.drops_by_luk / 100;

				//Drops affected by luk as a % increase [Skotlex]
				if (battle_config.drops_by_luk2)
					drop_rate += (int)(0.5 + drop_rate * status_get_luk(src) * battle_config.drops_by_luk2 / 10000.);

				if (sd != NULL) {
					int drop_rate_bonus = 100;

					// When PK Mode is enabled, increase item drop rate bonus of each items by 25% when there is a 20 level difference between the player and the monster.[KeiKun]
					if (battle_config.pk_mode && (md->level - sd->status.base_level >= 20))
						drop_rate_bonus += 25; // flat 25% bonus

					drop_rate_bonus += sd->dropaddrace[md->status.race] + (is_boss(src) ? sd->dropaddrace[RC_BOSS] : sd->dropaddrace[RC_NONBOSS]); // bonus2 bDropAddRace[KeiKun]

					if (sd->sc.data[SC_CASH_RECEIVEITEM] != NULL) // Increase drop rate if user has SC_CASH_RECEIVEITEM
						drop_rate_bonus += sd->sc.data[SC_CASH_RECEIVEITEM]->val1;

					if (sd->sc.data[SC_OVERLAPEXPUP] != NULL)
						drop_rate_bonus += sd->sc.data[SC_OVERLAPEXPUP]->val2;

					if (drop_rate_bonus != 100) {
						drop_rate = (int)(0.5 + drop_rate * drop_rate_bonus / 100.);
					}
				}
			}

#ifdef RENEWAL_DROP
			if (drop_modifier != 100) {
				drop_rate = drop_rate * drop_modifier / 100;
				if (drop_rate < 1)
					drop_rate = 1;
			}
#endif
			if (sd != NULL && sd->status.mod_drop != 100) {
				drop_rate = drop_rate * sd->status.mod_drop / 100;
				if (drop_rate < 1)
					drop_rate = 1;
			}

			if (battle_config.drop_rate0item)
				drop_rate = max(drop_rate, 0);
			else
				drop_rate = max(drop_rate, 1);

			// Make sure the bonuses don't make the drop rate grow past the configured threshold (unless it already was)
			drop_rate = min(drop_rate, max(md->db->dropitem[i].p, battle_config.item_drop_bonus_max_threshold));

			// attempt to drop the item
			if (rnd() % 10000 >= drop_rate)
					continue;

			if( mvp_sd && it->type == IT_PETEGG ) {
				pet->create_egg(mvp_sd, md->db->dropitem[i].nameid);
				continue;
			}

			ditem = mob->setdropitem(md->db->dropitem[i].nameid, md->db->dropitem[i].options, 1, it);

			// Official Drop Announce [Jedzkie]
			if (mvp_sd != NULL) {
				if ((id = itemdb->search(it->nameid)) != NULL && id->flag.drop_announce) {
					clif->item_drop_announce(mvp_sd, it->nameid, md->name);
				}
			}

			// Announce first, or else ditem will be freed. [Lance]
			// By popular demand, use base drop rate for autoloot code. [Skotlex]
			mob->item_drop(md, dlist, ditem, 0, battle_config.autoloot_adjust ? drop_rate : md->db->dropitem[i].p, homkillonly);
		}

		// Ore Discovery [Celest]
		if (sd == mvp_sd && pc->checkskill(sd,BS_FINDINGORE) > 0) {
			if( (temp = itemdb->chain_item(itemdb->chain_cache[ECC_ORE],&i)) ) {
				ditem = mob->setdropitem(temp, NULL, 1, NULL);
				mob->item_drop(md, dlist, ditem, 0, i, homkillonly);
			}
		}

		if(sd) {
			// process script-granted extra drop bonuses
			int itemid = 0;
			for (i = 0; i < ARRAYLENGTH(sd->add_drop) && (sd->add_drop[i].id != 0 || sd->add_drop[i].is_group); i++)
			{
				if ( sd->add_drop[i].race == -md->class_ ||
					( sd->add_drop[i].race > 0 && (
						sd->add_drop[i].race & map->race_id2mask(mstatus->race) ||
						sd->add_drop[i].race & map->race_id2mask((mstatus->mode&MD_BOSS) ? RC_BOSS : RC_NONBOSS)
					)))
				{
					//check if the bonus item drop rate should be multiplied with mob level/10 [Lupus]
					if(sd->add_drop[i].rate < 0) {
						//it's negative, then it should be multiplied. e.g. for Mimic,Myst Case Cards, etc
						// rate = base_rate * (mob_level/10) + 1
						drop_rate = -sd->add_drop[i].rate*(md->level/10)+1;
						drop_rate = cap_value(drop_rate, battle_config.item_drop_adddrop_min, battle_config.item_drop_adddrop_max);
						if (drop_rate > 10000) drop_rate = 10000;
					}
					else
						//it's positive, then it goes as it is
						drop_rate = sd->add_drop[i].rate;

					if (rnd()%10000 >= drop_rate)
						continue;
					itemid = (!sd->add_drop[i].is_group) ? sd->add_drop[i].id : itemdb->chain_item(sd->add_drop[i].id, &drop_rate);
					if( itemid )
						mob->item_drop(md, dlist, mob->setdropitem(itemid, NULL, 1, NULL), 0, drop_rate, homkillonly);
				}
			}

			// process script-granted zeny bonus (get_zeny_num) [Skotlex]
			if( sd->bonus.get_zeny_num && rnd()%100 < sd->bonus.get_zeny_rate ) {
				i = sd->bonus.get_zeny_num > 0 ? sd->bonus.get_zeny_num : -md->level * sd->bonus.get_zeny_num;
				if (!i) i = 1;
				pc->getzeny(sd, 1+rnd()%i, LOG_TYPE_PICKDROP_MONSTER, NULL);
			}
		}

		// process items looted by the mob
		if(md->lootitem) {
			for(i = 0; i < md->lootitem_count; i++)
				mob->item_drop(md, dlist, mob->setlootitem(&md->lootitem[i]), 1, 10000, homkillonly);
		}
		if (dlist->item) //There are drop items.
			timer->add(tick + (!battle_config.delay_battle_damage?500:0), mob->delay_item_drop, 0, (intptr_t)dlist);
		else //No drops
			ers_free(item_drop_list_ers, dlist);
	} else if (md->lootitem && md->lootitem_count) {
		//Loot MUST drop!
		struct item_drop_list *dlist = ers_alloc(item_drop_list_ers, struct item_drop_list);
		dlist->m = md->bl.m;
		dlist->x = md->bl.x;
		dlist->y = md->bl.y;
		dlist->first_charid = (mvp_sd ? mvp_sd->status.char_id : 0);
		dlist->second_charid = (second_sd ? second_sd->status.char_id : 0);
		dlist->third_charid = (third_sd ? third_sd->status.char_id : 0);
		dlist->item = NULL;
		for(i = 0; i < md->lootitem_count; i++)
			mob->item_drop(md, dlist, mob->setlootitem(&md->lootitem[i]), 1, 10000, homkillonly);
		timer->add(tick + (!battle_config.delay_battle_damage?500:0), mob->delay_item_drop, 0, (intptr_t)dlist);
	}

	if(mvp_sd && md->db->mexp > 0 && md->special_state.ai == AI_NONE) {
		int log_mvp[2] = {0};
		unsigned int mexp;
		int64 exp;

		//mapflag: noexp check [Lorky]
		if (map->list[m].flag.nobaseexp || type&2) {
			exp = 1;
		} else {
			exp = md->db->mexp;
			if (count > 1)
				exp += apply_percentrate64(exp, battle_config.exp_bonus_attacker * (count-1), 100); //[Gengar]
		}

		mexp = (unsigned int)cap_value(exp, 1, UINT_MAX);

		clif->mvp_effect(mvp_sd);
		clif->mvp_exp(mvp_sd,mexp);
		pc->gainexp(mvp_sd, &md->bl, mexp,0, false);
		log_mvp[1] = mexp;

		if (!(map->list[m].flag.nomvploot || type&1)) {
			/* pose them randomly in the list -- so on 100% drop servers it wont always drop the same item */
			struct mob_drop mdrop[MAX_MVP_DROP] = { { 0 } };

			for (i = 0; i < MAX_MVP_DROP; i++) {
				int rpos;
				if (md->db->mvpitem[i].nameid == 0)
					continue;
				do {
					rpos = rnd()%MAX_MVP_DROP;
				} while (mdrop[rpos].nameid != 0);

				mdrop[rpos].nameid = md->db->mvpitem[i].nameid;
				mdrop[rpos].p = md->db->mvpitem[i].p;
				mdrop[rpos].options = md->db->mvpitem[i].options;
			}

			for (i = 0; i < MAX_MVP_DROP; i++) {
				struct item_data *data = NULL;
				int rate = 0;

				if (mdrop[i].nameid <= 0)
					continue;
				if ((data = itemdb->exists(mdrop[i].nameid)) == NULL)
					continue;

				rate = mdrop[i].p;
				if (rate <= 0 && !battle_config.drop_rate0item)
					rate = 1;
				if (rate > rnd()%10000) {
					struct item item = { 0 };

					item.nameid = mdrop[i].nameid;
					item.identify = itemdb->isidentified2(data);
					if (mdrop[i].options != NULL)
						mob->setdropitem_options(&item, mdrop[i].options);
					clif->mvp_item(mvp_sd, item.nameid);
					log_mvp[0] = item.nameid;

					if((temp = pc->additem(mvp_sd,&item,1,LOG_TYPE_PICKDROP_PLAYER)) != 0) {
						clif->additem(mvp_sd,0,0,temp);
						map->addflooritem(&md->bl, &item, 1, mvp_sd->bl.m, mvp_sd->bl.x, mvp_sd->bl.y, mvp_sd->status.char_id, (second_sd?second_sd->status.char_id : 0), (third_sd ? third_sd->status.char_id : 0), 1, true);
					}

					//Logs items, MVP prizes [Lupus]
					logs->pick_mob(md, LOG_TYPE_MVP, -1, &item, data);
					break;
				}
			}
		}

		logs->mvpdrop(mvp_sd, md->class_, log_mvp);
	}

	if (type&2 && !sd && md->class_ == MOBID_EMPELIUM && md->guardian_data) {
		//Emperium destroyed by script. Discard mvp character. [Skotlex]
		mvp_sd = NULL;
	}

	rebirth =  ( md->sc.data[SC_KAIZEL] || (md->sc.data[SC_REBIRTH] && !md->state.rebirth) );
	if( !rebirth ) { // Only trigger event on final kill
		md->status.hp = 0; //So that npc_event invoked functions KNOW that mob is dead
		if( src ) {
			switch( src->type ) {
				case BL_PET: sd = BL_UCAST(BL_PET, src)->msd; break;
				case BL_HOM: sd = BL_UCAST(BL_HOM, src)->master; break;
				case BL_MER: sd = BL_UCAST(BL_MER, src)->master; break;
				case BL_ELEM: sd = BL_UCAST(BL_ELEM, src)->master; break;

				case BL_NUL:
				case BL_ITEM:
				case BL_SKILL:
				case BL_NPC:
				case BL_CHAT:
				case BL_PC:
				case BL_MOB:
				case BL_ALL:
					break;
			}
		}

		if( sd ) {
			if( sd->mission_mobid == md->class_) { //TK_MISSION [Skotlex]
				if (++sd->mission_count >= 100 && (temp = mob->get_random_id(0, 0xE, sd->status.base_level)) != 0) {
					pc->addfame(sd, RANKTYPE_TAEKWON, 1);
					sd->mission_mobid = temp;
					pc_setglobalreg(sd,script->add_variable("TK_MISSION_ID"), temp);
					sd->mission_count = 0;
					clif->mission_info(sd, temp, 0);
				}
				pc_setglobalreg(sd,script->add_variable("TK_MISSION_COUNT"), sd->mission_count);
			}

			if( sd->status.party_id )
				map->foreachinrange(quest->update_objective_sub, &md->bl, AREA_SIZE, BL_PC, sd->status.party_id, md);
			else if( sd->avail_quests )
				quest->update_objective(sd, md);

			if( sd->md && src && src->type != BL_HOM && mob->db(md->class_)->lv > sd->status.base_level/2 )
				mercenary->kills(sd->md);
		}

		if( md->npc_event[0] && !md->state.npc_killmonster ) {
			if( sd && battle_config.mob_npc_event_type ) {
				pc->setparam(sd, SP_KILLERRID, sd->bl.id);
				npc->event(sd,md->npc_event,0);
			} else if( mvp_sd ) {
				pc->setparam(mvp_sd, SP_KILLERRID, sd?sd->bl.id:0);
				npc->event(mvp_sd,md->npc_event,0);
			} else
				npc->event_do(md->npc_event);
		} else if( mvp_sd && !md->state.npc_killmonster ) {
			pc->setparam(mvp_sd, SP_KILLEDRID, md->class_);
			npc->script_event(mvp_sd, NPCE_KILLNPC); // PCKillNPC [Lance]
		}

		md->status.hp = 1;
	}

	if(md->deletetimer != INVALID_TIMER) {
		timer->delete(md->deletetimer,mob->timer_delete);
		md->deletetimer = INVALID_TIMER;
	}
	/**
	 * Only loops if necessary (e.g. a poring would never need to loop)
	 **/
	if( md->can_summon )
		mob->deleteslave(md);

	map->freeblock_unlock();

	if( !rebirth ) {

		if (pc->db_checkid(md->vd->class)) {
			// Player mobs are not removed automatically by the client.
			/* first we set them dead, then we delay the out sight effect */
			clif->clearunit_area(&md->bl,CLR_DEAD);
			clif->clearunit_delayed(&md->bl, CLR_OUTSIGHT,tick+3000);
		} else
			/**
			 * We give the client some time to breath and this allows it to display anything it'd like with the dead corpose
			 * For example, this delay allows it to display soul drain effect
			 **/
			clif->clearunit_delayed(&md->bl, CLR_DEAD, tick+250);

	}

	if(!md->spawn) //Tell status->damage to remove it from memory.
		return 5; // Note: Actually, it's 4. Oh well...

	// MvP tomb [GreenBox]
	if (battle_config.mvp_tomb_enabled && md->spawn->state.boss == BTYPE_MVP && map->list[md->bl.m].flag.notomb != 1)
		mob->mvptomb_create(md, mvp_sd ? mvp_sd->status.name : NULL, time(NULL));

	if( !rebirth ) {
		status->change_clear(&md->bl,1);
		mob->setdelayspawn(md); //Set respawning.
	}
	return 3; //Remove from map.
}

static void mob_revive(struct mob_data *md, unsigned int hp)
{
	int64 tick = timer->gettick();

	nullpo_retv(md);
	md->state.skillstate = MSS_IDLE;
	md->last_thinktime = tick;
	md->next_walktime = tick+rnd()%1000+MIN_RANDOMWALKTIME;
	md->last_linktime = tick;
	md->last_pcneartime = 0;
	memset(md->dmglog, 0, sizeof(md->dmglog)); // Reset the damage done on the rebirthed monster, otherwise will grant full exp + damage done. [Valaris]
	md->tdmg = 0;
	if (!md->bl.prev)
		map->addblock(&md->bl);
	clif->spawn(&md->bl);
	skill->unit_move(&md->bl,tick,1);
	mob->use_skill(md, tick, MSC_SPAWN);
	if (battle_config.show_mob_info&3)
		clif->blname_ack(0, &md->bl);
}

static int mob_guardian_guildchange(struct mob_data *md)
{
	struct guild *g;
	nullpo_ret(md);

	if (!md->guardian_data)
		return 0;

	if (md->guardian_data->castle->guild_id == 0) {
		//Castle with no owner? Delete the guardians.
		if (md->class_ == MOBID_EMPELIUM) {
			//But don't delete the emperium, just clear it's guild-data
			md->guardian_data->g = NULL;
		} else {
			if (md->guardian_data->number >= 0 && md->guardian_data->number < MAX_GUARDIANS && md->guardian_data->castle->guardian[md->guardian_data->number].visible)
				guild->castledatasave(md->guardian_data->castle->castle_id, 10+md->guardian_data->number, 0);
			unit->free(&md->bl,CLR_OUTSIGHT); //Remove guardian.
		}
		return 0;
	}

	g = guild->search(md->guardian_data->castle->guild_id);
	if (g == NULL) {
		//Properly remove guardian info from Castle data.
		ShowError("mob_guardian_guildchange: New Guild (id %d) does not exists!\n", md->guardian_data->castle->guild_id);
		if (md->guardian_data->number >= 0 && md->guardian_data->number < MAX_GUARDIANS)
			guild->castledatasave(md->guardian_data->castle->castle_id, 10+md->guardian_data->number, 0);
		unit->free(&md->bl,CLR_OUTSIGHT);
		return 0;
	}

	md->guardian_data->g = g;

	return 1;
}

/*==========================================
 * Pick a random class for the mob
 *------------------------------------------*/
static int mob_random_class(int *value, size_t count)
{
	nullpo_ret(value);

	// no count specified, look into the array manually, but take only max 5 elements
	if (count < 1) {
		count = 0;
		while(count < 5 && mob->db_checkid(value[count])) count++;
		if(count < 1) // nothing found
			return 0;
	} else {
		// check if at least the first value is valid
		if(mob->db_checkid(value[0]) == 0)
			return 0;
	}
	//Pick a random value, hoping it exists. [Skotlex]
	return mob->db_checkid(value[rnd()%count]);
}

/*==========================================
 * Change mob base class
 *------------------------------------------*/
static int mob_class_change(struct mob_data *md, int class_)
{

	int64 tick = timer->gettick(), c = 0;
	int i, hp_rate;

	nullpo_ret(md);

	if (md->bl.prev == NULL)
		return 0;

	// Disable class changing for some targets...
	if (md->guardian_data)
		return 0; // Guardians/Emperium

	if (mob_is_treasure(md))
		return 0; // Treasure Boxes

	if (md->special_state.ai > AI_ATTACK)
		return 0; // Marine Spheres and Floras.

	if (mob->is_clone(md->class_))
		return 0; // Clones

	if (md->class_ == class_)
		return 0; // Nothing to change.

	hp_rate = get_percentage(md->status.hp, md->status.max_hp);
	md->class_ = class_;
	md->db = mob->db(class_);
	if (battle_config.override_mob_names == 1)
		memcpy(md->name, md->db->name, NAME_LENGTH);
	else
		memcpy(md->name, md->db->jname, NAME_LENGTH);

	mob_stop_attack(md);
	mob_stop_walking(md, STOPWALKING_FLAG_NONE);
	unit->skillcastcancel(&md->bl, 0);
	status->set_viewdata(&md->bl, class_);
	clif->class_change(&md->bl, md->vd->class, 1, NULL);
	status_calc_mob(md, SCO_FIRST);
	md->ud.state.speed_changed = 1; //Speed change update.

	if (battle_config.monster_class_change_recover) {
		memset(md->dmglog, 0, sizeof(md->dmglog));
		md->tdmg = 0;
	} else {
		md->status.hp = md->status.max_hp*hp_rate/100;
		if(md->status.hp < 1) md->status.hp = 1;
	}

	for(i=0,c=tick-MOB_MAX_DELAY;i<MAX_MOBSKILL;i++)
		md->skilldelay[i] = c;

	if(md->lootitem == NULL && md->db->status.mode&MD_LOOTER)
		md->lootitem=(struct item *)aCalloc(LOOTITEM_SIZE,sizeof(struct item));

	//Targets should be cleared no morph
	md->target_id = md->attacked_id = 0;

	//Need to update name display.
	clif->blname_ack(0, &md->bl);
	status_change_end(&md->bl,SC_KEEPING,INVALID_TIMER);
	return 0;
}

/*==========================================
 * mob heal, update display hp info of mob for players
 *------------------------------------------*/
static void mob_heal(struct mob_data *md, unsigned int heal)
{
	nullpo_retv(md);
	if (battle_config.show_mob_info&3)
		clif->blname_ack(0, &md->bl);
#if PACKETVER >= 20131223
	// Resend ZC_NOTIFY_MOVEENTRY to Update the HP
	if (clif->show_monster_hp_bar(&md->bl))
		clif->set_unit_walking(&md->bl, NULL, unit->bl2ud(&md->bl), AREA);
#endif

#if (PACKETVER >= 20120404 && PACKETVER < 20131223)
	if (battle_config.show_monster_hp_bar && !(md->status.mode&MD_BOSS)) {
		int i;
		for(i = 0; i < DAMAGELOG_SIZE; i++){ // must show hp bar to all char who already hit the mob.
			if (md->dmglog[i].id) {
				struct map_session_data *sd = map->charid2sd(md->dmglog[i].id);
				if (sd && check_distance_bl(&md->bl, &sd->bl, AREA_SIZE)) // check if in range
					clif->monster_hp_bar(md, sd);
			}
		}
	}
#endif
}

/*==========================================
 * Added by RoVeRT
 *------------------------------------------*/
static int mob_warpslave_sub(struct block_list *bl, va_list ap)
{
	struct mob_data *md = NULL;
	struct block_list *master;
	short x,y,range=0;
	master = va_arg(ap, struct block_list*);
	range = va_arg(ap, int);

	nullpo_ret(bl);
	nullpo_ret(master);
	Assert_ret(bl->type == BL_MOB);
	md = BL_UCAST(BL_MOB, bl);

	if(md->master_id!=master->id)
		return 0;

	map->search_free_cell(master, 0, &x, &y, range, range, SFC_DEFAULT);
	unit->warp(&md->bl, master->m, x, y,CLR_TELEPORT);
	return 1;
}

/*==========================================
 * Added by RoVeRT
 * Warps slaves. Range is the area around the master that they can
 * appear in randomly.
 *------------------------------------------*/
static int mob_warpslave(struct block_list *bl, int range)
{
	nullpo_ret(bl);
	if (range < 1)
		range = 1; //Min range needed to avoid crashes and stuff. [Skotlex]

	return map->foreachinmap(mob->warpslave_sub, bl->m, BL_MOB, bl, range);
}

/*==========================================
 *  Counts slave sub, currently checking if mob master is the given ID.
 *------------------------------------------*/
static int mob_countslave_sub(struct block_list *bl, va_list ap)
{
	int id = va_arg(ap, int);
	struct mob_data *md = NULL;

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_MOB);
	md = BL_UCAST(BL_MOB, bl);

	if (md->master_id == id)
		return 1;
	return 0;
}

/*==========================================
 * Counts the number of slaves a mob has on the map.
 *------------------------------------------*/
static int mob_countslave(struct block_list *bl)
{
	nullpo_ret(bl);
	return map->foreachinmap(mob->countslave_sub, bl->m, BL_MOB,bl->id);
}

/**
 * Summons amount slaves contained in the value[5] array using round-robin.
 *
 * @param md2 The mob which summons the slaves.
 * @param value Array with slave mob IDs.
 * @param amount The amount of slaves to spawn.
 * @param skill_id The Id of the skill which summons the slaves.
 * @return 1 on success, 0 on failure.
 *
 * @author Skotlex
 *
 **/
static int mob_summonslave(struct mob_data *md2, int *value, int amount, uint16 skill_id)
{
	nullpo_ret(md2);
	nullpo_ret(value);

	struct spawn_data data;

	memset(&data, 0, sizeof(struct spawn_data));
	data.m = md2->bl.m;
	data.x = md2->bl.x;
	data.y = md2->bl.y;
	data.num = 1;
	data.state.size = md2->special_state.size;
	data.state.ai = md2->special_state.ai;

	if (mob->db_checkid(value[0]) == 0)
		return 0;

	md2->can_summon = 1; /// Flags this monster is able to summon; saves a worth amount of memory upon deletion.

	int count = 0;

	while (count < 5 && mob->db_checkid(value[count]) != 0)
		count++;

	if (count < 1)
		return 0;

	int k = 0;

	if (amount > 0 && amount < count) { /// Do not start on 0, pick some random sub subset. [Skotlex]
		k = rnd() % count;
		amount += k; /// Increase final value by same amount to preserve total number to summon.
	}

	int hp_rate = 0;

	if ((skill_id == NPC_TRANSFORMATION || skill_id == NPC_METAMORPHOSIS) &&
	    battle_config.monster_class_change_recover == 0)
		hp_rate = get_percentage(md2->status.hp, md2->status.max_hp);

	for (; k < amount; k++) {
		data.class_ = value[k % count]; /// Summon slaves in round-robin fashion. [Skotlex]

		if (mob->db_checkid(data.class_) == 0)
			continue;

		short x;
		short y;
		map->search_free_cell(&md2->bl, 0, &x, &y, MOB_SLAVEDISTANCE, MOB_SLAVEDISTANCE, SFC_DEFAULT);
		data.x = x;
		data.y = y;

		if (battle_config.override_mob_names == 1)
			strcpy(data.name, DEFAULT_MOB_NAME);
		else
			strcpy(data.name, DEFAULT_MOB_JNAME);

		if (mob->parse_dataset(&data) == 0)
			continue;

		struct mob_data *md = mob->spawn_dataset(&data, 0);

		if (skill_id == NPC_SUMMONSLAVE) {
			md->master_id = md2->bl.id;
			md->special_state.ai = md2->special_state.ai;
		}

		mob->spawn(md);

		if (hp_rate > 0) /// Scale HP.
			md->status.hp = md->status.max_hp * hp_rate / 100;

		/** Inherit the aggressive mode of the master. **/
		if (battle_config.slaves_inherit_mode > 0 && md->master_id > 0) {
			switch (battle_config.slaves_inherit_mode) {
			case 1: /// Always aggressive.
				if ((md->status.mode & MD_AGGRESSIVE) == 0)
					sc_start4(NULL, &md->bl, SC_MODECHANGE, 100, 1, 0, MD_AGGRESSIVE, 0, 0, 0);

				break;
			case 2: /// Always passive.
				if ((md->status.mode & MD_AGGRESSIVE) == MD_AGGRESSIVE)
					sc_start4(NULL, &md->bl, SC_MODECHANGE, 100, 1, 0, 0, MD_AGGRESSIVE, 0, 0);

				break;
			default: /// Copy master.
				if ((md2->status.mode & MD_AGGRESSIVE) == MD_AGGRESSIVE)
					sc_start4(NULL, &md->bl, SC_MODECHANGE, 100, 1, 0, MD_AGGRESSIVE, 0, 0, 0);
				else
					sc_start4(NULL, &md->bl, SC_MODECHANGE, 100, 1, 0, 0, MD_AGGRESSIVE, 0, 0);
				break;
			}
		}

		clif->skill_nodamage(&md->bl, &md->bl, skill_id, amount, 1);
	}

	return 1;
}

/*==========================================
 * MOBskill lookup (get skillindex through skill_id)
 * Returns INDEX_NOT_FOUND if not found.
 *------------------------------------------*/
static int mob_skill_id2skill_idx(int class_, uint16 skill_id)
{
	int i, max = mob->db(class_)->maxskill;
	struct mob_skill *ms=mob->db(class_)->skill;

	if (ms == NULL)
		return INDEX_NOT_FOUND;

	ARR_FIND(0, max, i, ms[i].skill_id == skill_id);
	if (i == max)
		return INDEX_NOT_FOUND;
	return i;
}

/*==========================================
 * Friendly Mob whose HP is decreasing by a nearby MOB is looked for.
 *------------------------------------------*/
static int mob_getfriendhprate_sub(struct block_list *bl, va_list ap)
{
	int min_rate, max_rate,rate;
	struct block_list **fr;
	struct mob_data *md;

	md = va_arg(ap,struct mob_data *);
	nullpo_ret(bl);
	nullpo_ret(md);
	min_rate=va_arg(ap,int);
	max_rate=va_arg(ap,int);
	fr=va_arg(ap,struct block_list **);

	if( md->bl.id == bl->id && !(battle_config.mob_ai&0x10))
		return 0;

	if ((*fr) != NULL) //A friend was already found.
		return 0;

	if (battle->check_target(&md->bl,bl,BCT_ENEMY)>0)
		return 0;

	rate = get_percentage(status_get_hp(bl), status_get_max_hp(bl));

	if (rate >= min_rate && rate <= max_rate)
		(*fr) = bl;
	return 1;
}
static struct block_list *mob_getfriendhprate(struct mob_data *md, int min_rate, int max_rate)
{
	struct block_list *fr=NULL;
	int type = BL_MOB;

	nullpo_retr(NULL, md);

	if (md->special_state.ai != AI_NONE) //Summoned creatures. [Skotlex]
		type = BL_PC;

	map->foreachinrange(mob->getfriendhprate_sub, &md->bl, 8, type,md,min_rate,max_rate,&fr);
	return fr;
}
/*==========================================
 * Check hp rate of its master
 *------------------------------------------*/
static struct block_list *mob_getmasterhpltmaxrate(struct mob_data *md, int rate)
{
	if( md && md->master_id > 0 ) {
		struct block_list *bl = map->id2bl(md->master_id);
		if( bl && get_percentage(status_get_hp(bl), status_get_max_hp(bl)) < rate )
			return bl;
	}

	return NULL;
}

/**
 * Checks if the passed monster/character meets the passed status change requirements
 * and returns it by reference parameter on success.
 *
 * @param bl The monster/character to check.
 * @param ap List of arguments. (Source monster, MSC_* flag, SC_* flag, reference bl.)
 * @return Always 0.
 *
 **/
static int mob_getfriendstatus_sub(struct block_list *bl, va_list ap)
{
	nullpo_ret(bl);

	struct mob_data *md = va_arg(ap, struct mob_data *);

	nullpo_ret(md);

	if (md->bl.id == bl->id && (battle_config.mob_ai & 0x10) == 0)
		return 0;

	if (battle->check_target(&md->bl, bl, BCT_ENEMY) > 0)
		return 0;

	int cond1 = va_arg(ap, int);
	int cond2 = va_arg(ap, int);
	struct block_list **fr = va_arg(ap, struct block_list **);

	if ((*fr) != NULL) // A friend was already found.
		return 0;

	int flag = 0;
	struct status_change *sc = status->get_sc(bl);

	if (cond2 == -1) { // Check for any of the common status alignments.
		for (int i = SC_COMMON_MIN; i <= SC_COMMON_MAX; i++) {
			if ((flag = (sc->data[i] != NULL)) != 0) // Once an effect was found, break out. [Skotlex]
				break;
		}
	} else {
		flag = (sc->data[cond2] != NULL);
	}

	if ((flag ^ (cond1 == MSC_FRIENDSTATUSOFF)) != 0)
		(*fr) = bl;

	return 0;
}

/**
 * Gets a random monster/character within a range of 8 cells around md.
 * If md is summoned (no monster slave!) a character is picker, otherwise a monster.
 *
 * @param md The monster which tries to cast a skill.
 * @param cond1 Whether to check for active or inactive status change. (MSC_FRIENDSTATUSON/MSC_FRIENDSTATUSOFF)
 * @param cond2 The status change (SC_* flag) to check.
 * @return A randomly picked monster/character within range.
 *
 **/
static struct block_list *mob_getfriendstatus(struct mob_data *md, int cond1, int cond2)
{
	nullpo_ret(md);

	int type = (md->special_state.ai != AI_NONE) ? BL_PC : BL_MOB;
	struct block_list *fr = NULL;

	map->foreachinrange(mob->getfriendstatus_sub, &md->bl, 8, type, md, cond1, cond2, &fr);

	return fr;
}

/**
 * Checks if skill cast condition is fulfilled and executes the skill in case of success.
 *
 * @param md The monster which tries to cast a skill.
 * @param tick The timestamp of skill execution.
 * @param event The MSC_* flag which triggered the skill execution. (-1 for non-event skill conditions.)
 * @return 0 on success, 1 on failure.
 *
 **/
static int mob_use_skill(struct mob_data *md, int64 tick, int event)
{
	GUARD_MAP_LOCK

	nullpo_retr(1, md);

	struct mob_skill *ms = md->db->skill;

	nullpo_retr(1, ms);

	if (battle_config.mob_skill_rate == 0 || md->ud.skilltimer != INVALID_TIMER || md->db->maxskill == 0)
		return 1;

	if (event == -1 && DIFF_TICK(md->ud.canact_tick, tick) > 0)
		return 1; // Skill act delay only affects non-event skill conditions.

	// Pick a starting position and loop from that.
	int skill_idx = ((battle_config.mob_ai & 0x100) != 0) ? rnd() % md->db->maxskill : 0;

	for (int i = 0; i < md->db->maxskill; skill_idx++, i++) {
		if (skill_idx == md->db->maxskill)
			skill_idx = 0;

		if (DIFF_TICK(tick, md->skilldelay[skill_idx]) < ms[skill_idx].delay)
			continue;

		enum MobSkillState state = ms[skill_idx].state;

		if (state != md->state.skillstate) {
			bool state_dead = (md->state.skillstate == MSS_DEAD);
			bool any_target = (state == MSS_ANYTARGET && md->target_id != 0 && md->state.skillstate != MSS_LOOT);

			// MSS_ANYTARGET works with any state as long as there's a target. [Skotlex]
			if (state_dead || (state != MSS_ANY && !any_target))
				continue;
		}

		if (rnd() % 10000 > ms[skill_idx].permillage)
			continue;

		int cast_cond = ms[skill_idx].cond1;
		int cond_data = ms[skill_idx].cond2;
		int flag = 0;
		struct block_list *fbl = NULL; // Friend bl, which can either be a BL_PC or BL_MOB depending on the situation. [Skotlex]

		if (cast_cond == event) {
			flag = 1; // Trigger skill.
		} else if (cast_cond == MSC_SKILLUSED) {
			flag = ((event & 0xFFFF) == MSC_SKILLUSED && ((event >> 16) == cond_data || cond_data == 0));
		} else if (event == -1) {
			// Avoid entering on defined events to avoid "hyper-active skill use" due to the overflow of calls to this function in battle.
			switch (cast_cond) {
			case MSC_ALWAYS:
				flag = 1;
				break;
			case MSC_MYHPLTMAXRATE: // HP <= x%
				flag = get_percentage(md->status.hp, md->status.max_hp);
				flag = (flag <= cond_data);
				break;
			case MSC_MYHPINRATE: // HP >= x% && HP <= y%
				flag = get_percentage(md->status.hp, md->status.max_hp);
				flag = (flag >= cond_data && flag <= ms[skill_idx].val[0]);
				break;
			case MSC_MYSTATUSON: // Status change x is active.
			case MSC_MYSTATUSOFF: // Status change x is inactive.
				if (cond_data == -1) { // Check for any of the common status alignments.
					for (int j = SC_COMMON_MIN; j <= SC_COMMON_MAX; j++) {
						if ((flag = (md->sc.data[j] != NULL)) != 0)
							break;
					}
				} else {
					flag = (md->sc.data[cond_data] != NULL);
				}

				flag ^= (cast_cond == MSC_MYSTATUSOFF);
				break;
			case MSC_FRIENDHPLTMAXRATE: // FriendHP <= x%
				flag = ((fbl = mob->getfriendhprate(md, 0, cond_data)) != NULL);
				break;
			case MSC_FRIENDHPINRATE: // FriendHP >= x% && FriendHP <= y%
				flag = ((fbl = mob->getfriendhprate(md, cond_data, ms[skill_idx].val[0])) != NULL);
				break;
			case MSC_FRIENDSTATUSON: // Friend's status change x is active.
			case MSC_FRIENDSTATUSOFF: // Friend's status change x is inactive.
				flag = ((fbl = mob->getfriendstatus(md, cast_cond, cond_data)) != NULL);
				break;
			case MSC_SLAVELT: // Monster has less than x active slaves.
				flag = (mob->countslave(&md->bl) < cond_data);
				break;
			case MSC_ATTACKPCGT: // Monster is attacked by more than x units.
				flag = (unit->counttargeted(&md->bl) > cond_data);
				break;
			case MSC_SLAVELE: // Monster has x or less active slaves.
				flag = (mob->countslave(&md->bl) <= cond_data);
				break;
			case MSC_ATTACKPCGE: // Monster is attacked by x or more units.
				flag = (unit->counttargeted(&md->bl) >= cond_data);
				break;
			case MSC_AFTERSKILL: // Monster used skill x, or any skill if x is 0.
				flag = (md->ud.skill_id == cond_data || cond_data == 0);
				break;
			case MSC_RUDEATTACKED: // Monster was rude attacked RUDE_ATTACKED_COUNT or more times.
				flag = (md->state.attacked_count >= RUDE_ATTACKED_COUNT);

				// Rude attacked count should be reset after the skill condition is met. Thanks to Komurka [Skotlex]
				if (flag)
					md->state.attacked_count = 0;

				break;
			case MSC_MASTERHPLTMAXRATE: // MasterHP < x%
				flag = ((fbl = mob->getmasterhpltmaxrate(md, cond_data)) != NULL);
				break;
			case MSC_MASTERATTACKED: // Monster's master is under attack.
				flag = (md->master_id > 0 && (fbl = map->id2bl(md->master_id)) != NULL);
				flag = (fbl != NULL && unit->counttargeted(fbl) > 0);
				break;
			case MSC_ALCHEMIST: // Monster was summoned by an Alchemist.
				flag = (md->state.alchemist != 0);
				break;
			}
		}

		if (flag == 0) // Skill cast condition not fulfilled.
			continue;

		// Execute skill.
		if (skill->get_casttype(ms[skill_idx].skill_id) == CAST_GROUND) { // Ground skill.
			int target_type = ms[skill_idx].target;
			int skill_range = skill->get_range2(&md->bl, ms[skill_idx].skill_id, ms[skill_idx].skill_lv);
			struct block_list *bl;

			switch (target_type) {
			case MST_RANDOM: // Pick a random enemy within skill range. Skill center is monster position.
				bl = battle->get_enemy(&md->bl, DEFAULT_ENEMY_TYPE(md), skill_range);
				break;
			case MST_TARGET: // Monster's current target is within skill range. Skill center is monster position.
			case MST_AROUND5: // Monster's current target is within skill range. Skill center is a random cell within a range of 1.
			case MST_AROUND6: // Monster's current target is within skill range. Skill center is a random cell within a range of 2.
			case MST_AROUND7: // Monster's current target is within skill range. Skill center is a random cell within a range of 3.
			case MST_AROUND8: // Monster's current target is within skill range. Skill center is a random cell within a range of 4.
				bl = map->id2bl(md->target_id);
				break;
			case MST_MASTER: // Monster's master is within skill range. Skill center is monster position.
				// If monster has no master, use the monster as target,
				bl = (md->master_id != 0) ? map->id2bl(md->master_id) : &md->bl;

				if (bl != NULL)
					break;

				// If monster has a master but master wasn't found, try a friend.
				FALLTHROUGH
			case MST_FRIEND: // Monster's friend is within skill range. Skill center is monster position.
				bl = (fbl != NULL) ? fbl : &md->bl;
				break;
			default: // Monster is within skill range. Skill center is monster position.
				bl = &md->bl;
				break;
			}

			if (bl == NULL) // No target found.
				continue;

			short x = bl->x;
			short y = bl->y;

			// Find a target cell.
			if (target_type >= MST_AROUND5 && target_type <= MST_AROUND) {
				int range = target_type - ((target_type >= MST_AROUND1) ? MST_AROUND1 : MST_AROUND5) + 1;
				map->search_free_cell(&md->bl, md->bl.m, &x, &y, range, range, (SFC_XY_CENTER | SFC_REACHABLE));
			}

			md->skill_idx = skill_idx;
			map->freeblock_lock();

			uint16 sk_id = ms[skill_idx].skill_id;
			uint16 sk_lv = ms[skill_idx].skill_lv;
			int casttime = ms[skill_idx].casttime;
			short cancel = ms[skill_idx].cancel;

			if (!battle->check_range(&md->bl, bl, skill_range)
			    || unit->skilluse_pos2(&md->bl, x, y, sk_id, sk_lv, casttime, cancel) == 0) {
				map->freeblock_unlock();
				continue;
			}
		} else { // Targeted skill.
			int skill_range = skill->get_range2(&md->bl, ms[skill_idx].skill_id, ms[skill_idx].skill_lv);
			struct block_list *bl;

			switch (ms[skill_idx].target) {
			case MST_RANDOM: // Pick a random enemy within skill range.
				bl = battle->get_enemy(&md->bl, DEFAULT_ENEMY_TYPE(md), skill_range);
				break;
			case MST_TARGET: // Monster's current target is within skill range.
				bl = map->id2bl(md->target_id);
				break;
			case MST_MASTER: // Monster's master is within skill range.
				// If monster has no master, use the monster as target,
				bl = (md->master_id != 0) ? map->id2bl(md->master_id) : &md->bl;

				if (bl != NULL)
					break;

				// If monster has a master but master wasn't found, try a friend.
				FALLTHROUGH
			case MST_FRIEND: // Monster's friend is within skill range.
				bl = (fbl != NULL) ? fbl : &md->bl;
				break;
			default: // Monster is within skill range.
				bl = &md->bl;
				break;
			}

			if (bl == NULL) // No target found.
				continue;

			md->skill_idx = skill_idx;
			map->freeblock_lock();

			uint16 sk_id = ms[skill_idx].skill_id;
			uint16 sk_lv = ms[skill_idx].skill_lv;
			int casttime = ms[skill_idx].casttime;
			short cancel = ms[skill_idx].cancel;

			if (!battle->check_range(&md->bl, bl, skill_range)
			    || unit->skilluse_id2(&md->bl, bl->id, sk_id, sk_lv, casttime, cancel) == 0) {
				map->freeblock_unlock();
				continue;
			}
		}

		// Skill used.
		if (ms[skill_idx].msg_id != 0) { // Display color message. [SnakeDrak]
			char temp[CHAT_SIZE_MAX + NAME_LENGTH + 10];
			char name[NAME_LENGTH];
			struct mob_chat *mc = mob->chat(ms[skill_idx].msg_id);

			snprintf(name, sizeof(name), "%s", md->name);
			strtok(name, "#"); // Discard extra name identifier if present. [Daegaladh]
			snprintf(temp, sizeof(temp), "%s : %s", name, mc->msg);
			clif->messagecolor(&md->bl, mc->color, temp);
		}

		if ((battle_config.mob_ai & 0x200) == 0) { // Pass on delay to same skill.
			for (int j = 0; j < md->db->maxskill; j++) {
				if (md->db->skill[j].skill_id == ms[skill_idx].skill_id)
					md->skilldelay[j] = tick;
			}
		} else {
			md->skilldelay[skill_idx] = tick;
		}

		map->freeblock_unlock();
		return 0;
	}

	// No skill was used.
	md->skill_idx = -1;
	return 1;
}

/*==========================================
 * Skill use event processing
 *------------------------------------------*/
static int mob_use_skill_event(struct mob_data *md, struct block_list *src, int64 tick, int flag)
{
	int target_id;

	nullpo_retr(1, md);
	nullpo_retr(1, src);
	if(md->bl.prev == NULL || md->status.hp <= 0)
		return 1;

	if (md->special_state.ai == AI_SPHERE) {//LOne WOlf explained that ANYONE can trigger the marine countdown skill. [Skotlex]
		md->state.alchemist = 1;
		return mob->use_skill(md, timer->gettick(), MSC_ALCHEMIST);
	}

	target_id = md->target_id;
	if (!target_id || battle_config.mob_changetarget_byskill)
		md->target_id = src->id;

	int res = 1;

	if (flag == -1)
		res = mob->use_skill(md, tick, MSC_CASTTARGETED);
	else if ((flag&0xffff) == MSC_SKILLUSED)
		res = mob->use_skill(md, tick, flag);
	else if (flag&BF_SHORT)
		res = mob->use_skill(md, tick, MSC_CLOSEDATTACKED);
	else if (flag&BF_LONG && !(flag&BF_MAGIC)) //Long-attacked should not include magic.
		res = mob->use_skill(md, tick, MSC_LONGRANGEATTACKED);
	else if ((flag & BF_MAGIC) != 0)
		res = mob->use_skill(md, tick, MSC_MAGICATTACKED);

	if (res != 0)
	//Restore previous target only if skill condition failed to trigger. [Skotlex]
		md->target_id = target_id;
	//Otherwise check if the target is an enemy, and unlock if needed.
	else if (battle->check_target(&md->bl, src, BCT_ENEMY) <= 0)
		md->target_id = target_id;

	return res;
}

// Player cloned mobs. [Valaris]
static int mob_is_clone(int class_)
{
	if(class_ < MOB_CLONE_START || class_ > MOB_CLONE_END)
		return 0;
	if (mob->db(class_) == mob->dummy)
		return 0;
	return class_;
}

/**
 * Spawns a mob which is a clone of another character.
 *
 * @param sd The character which should be cloned.
 * @param m The ID of the map where the clone should be spawned.
 * @param x The x coordinate where the clone should be spawned.
 * @param y The y coordinate where the clone should be spawned.
 * @param event The name of the event which should be executed when the clone is killed.
 * @param master_id If passed, the clone will be attached to this account ID.
 * @param mode The clone's mob mode(s). (Defaults to MD_CANMOVE|MD_AGGRESSIVE|MD_ASSIST|MD_CANATTACK.)
 * @param flag 0 - target characters; 1 - target mobs.
 * @param duration How long the clone will live before it is auto-removed. (ms)
 * @return The spawned clone's GID, or 0 if spawning failed.
 *
 **/
static int mob_clone_spawn(struct map_session_data *sd, int16 m, int16 x, int16 y, const char *event, int master_id,
			   uint32 mode, int flag, unsigned int duration)
{
	nullpo_ret(sd);

	if (pc_isdead(sd) && master_id != 0 && flag == 1)
		return 0;

	int class_;

	ARR_FIND(MOB_CLONE_START, MOB_CLONE_END, class_, mob->db_data[class_] == NULL);

	if (class_ < 0 || class_ >= MOB_CLONE_END)
		return 0;

	mob->db_data[class_] = (struct mob_db*)aCalloc(1, sizeof(struct mob_db));

	struct mob_db *db = mob->db_data[class_];
	struct status_data *mstatus = &db->status;

	strcpy(db->sprite, sd->status.name);
	strcpy(db->name, sd->status.name);
	strcpy(db->jname, sd->status.name);
	db->lv = status->get_lv(&sd->bl);
	db->dmg_taken_rate = 100;
	memcpy(mstatus, &sd->base_status, sizeof(struct status_data));
	mstatus->rhw.atk2 = mstatus->dex + mstatus->rhw.atk + mstatus->rhw.atk2; /// Max ATK.
	mstatus->rhw.atk = mstatus->dex; /// Min ATK.

	if (mstatus->lhw.atk > 0) {
		mstatus->lhw.atk2 = mstatus->dex + mstatus->lhw.atk + mstatus->lhw.atk2; /// Max ATK.
		mstatus->lhw.atk = mstatus->dex; /// Min ATK.
	}

	if (mode != MD_NONE) /// User provided mode.
		mstatus->mode = mode;
	else if (flag == 1) /// Friendly Character, remove looting.
		mstatus->mode &= ~MD_LOOTER;

	mstatus->hp = mstatus->max_hp;
	mstatus->sp = mstatus->max_sp;
	memcpy(&db->vd, &sd->vd, sizeof(struct view_data));
	db->base_exp = 1;
	db->job_exp = 1;
	db->range2 = AREA_SIZE; /// Let them have the same view-range as players.
	db->range3 = AREA_SIZE; /// Min chase of a screen.
	db->option = sd->sc.option;

	const int fd = sd->fd;

	sd->fd = 0; /// Temporarily disable sd's fd so it doesn't receive the messages from skill_check_condition_castbegin.

	struct mob_skill *mob_skills = &db->skill[0];

	/// Go Backwards to give better priority to advanced skills.
	for (int i = 0, j = MAX_SKILL_TREE - 1; j >= 0 && i < MAX_MOBSKILL; j--) {
		const int idx = pc->skill_tree[pc->class2idx(sd->status.class)][j].idx;
		const int skill_id = pc->skill_tree[pc->class2idx(sd->status.class)][j].id;

		if (skill_id == 0 || sd->status.skill[idx].lv < 1 ||
		    (skill->dbs->db[idx].inf2 & (INF2_WEDDING_SKILL | INF2_GUILD_SKILL)) > 0)
			continue;

		int h;

		for (h = 0; h < map->list[sd->bl.m].zone->disabled_skills_count; h++) {
			if (skill_id == map->list[sd->bl.m].zone->disabled_skills[h]->nameid &&
			    map->list[sd->bl.m].zone->disabled_skills[h]->subtype == MZS_CLONE)
				break;
		}

		if (h < map->list[sd->bl.m].zone->disabled_skills_count)
			continue;

		/// Normal aggressive mob. Disable skills that cannot help fighting against players. (Those with flags UF_NOMOB and UF_NOPC are specific to always aid players!) [Skotlex]
		if (flag == 0 && skill->get_unit_id(skill_id, sd->status.skill[idx].lv, 0) != 0 &&
		    (skill->get_unit_flag(skill_id) & (UF_NOMOB | UF_NOPC)) > 0)
			continue;

		/// The clone should be able to cast the skill. (E.g. have the required weapon.) [bugreport:5299]
		if (skill->check_condition_castbegin(sd, skill_id, sd->status.skill[idx].lv) == 0)
			continue;

		memset(&mob_skills[i], 0, sizeof(struct mob_skill));
		mob_skills[i].skill_id = skill_id;
		mob_skills[i].skill_lv = sd->status.skill[idx].lv;
		mob_skills[i].state = MSS_ANY;
		mob_skills[i].permillage = 500 * battle_config.mob_skill_rate / 100; /// Default chance of all skills: 5%
		mob_skills[i].emotion = -1;
		mob_skills[i].cancel = 0;
		mob_skills[i].casttime = skill->cast_fix(&sd->bl, skill_id, mob_skills[i].skill_lv);
		mob_skills[i].delay = 5000 + skill->delay_fix(&sd->bl, skill_id, mob_skills[i].skill_lv);

		const int inf = skill->dbs->db[idx].inf;

		if ((inf & INF_ATTACK_SKILL) == INF_ATTACK_SKILL) {
			mob_skills[i].target = MST_TARGET;
			mob_skills[i].cond1 = MSC_ALWAYS;

			if (skill->get_range(skill_id, mob_skills[i].skill_lv) > 3)
				mob_skills[i].state = MSS_ANYTARGET;
			else
				mob_skills[i].state = MSS_BERSERK;
		} else if ((inf & INF_GROUND_SKILL) == INF_GROUND_SKILL) {
			if ((skill->get_inf2(skill_id) & INF2_TRAP) == INF2_TRAP) { /// Traps!
				mob_skills[i].state = MSS_IDLE;
				mob_skills[i].target = MST_AROUND2;
				mob_skills[i].delay = 60000;
			} else if (skill->get_unit_target(skill_id, sd->status.skill[idx].lv) == BCT_ENEMY) { /// Target Enemy.
				mob_skills[i].state = MSS_ANYTARGET;
				mob_skills[i].target = MST_TARGET;
				mob_skills[i].cond1 = MSC_ALWAYS;
			} else { /// Target allies.
				mob_skills[i].target = MST_FRIEND;
				mob_skills[i].cond1 = MSC_FRIENDHPLTMAXRATE;
				mob_skills[i].cond2 = 95;
			}
		} else if ((inf & INF_SELF_SKILL) == INF_SELF_SKILL) {
			if ((skill->get_inf2(skill_id) & INF2_NO_TARGET_SELF) == INF2_NO_TARGET_SELF) { /// Auto-select target skill.
				mob_skills[i].target = MST_TARGET;
				mob_skills[i].cond1 = MSC_ALWAYS;

				if (skill->get_range(skill_id, mob_skills[i].skill_lv) > 3)
					mob_skills[i].state = MSS_ANYTARGET;
				else
					mob_skills[i].state = MSS_BERSERK;
			} else { /// Self skill.
				mob_skills[i].target = MST_SELF;
				mob_skills[i].cond1 = MSC_MYHPLTMAXRATE;
				mob_skills[i].cond2 = 90;
				mob_skills[i].permillage = 2000;

				const int time1 = skill->get_time(skill_id, mob_skills[i].skill_lv);
				const int time2 = skill->get_time2(skill_id, mob_skills[i].skill_lv);

				/** Delay: Remove the stock 5 secs and add half of the support time. **/
				mob_skills[i].delay += -5000 + (time1 + time2) / 2;

				if (mob_skills[i].delay < 5000)
					mob_skills[i].delay = 5000; /// With a minimum of 5 seconds.
			}
		} else if ((inf & INF_SUPPORT_SKILL) == INF_SUPPORT_SKILL) {
			mob_skills[i].target = MST_FRIEND;
			mob_skills[i].cond1 = MSC_FRIENDHPLTMAXRATE;
			mob_skills[i].cond2 = 90;

			if (skill_id == AL_HEAL)
				mob_skills[i].permillage = 5000; /// Higher skill rate usage for heal.
			else if (skill_id == ALL_RESURRECTION)
				mob_skills[i].cond2 = 1;

			const int time1 = skill->get_time(skill_id, mob_skills[i].skill_lv);
			const int time2 = skill->get_time2(skill_id, mob_skills[i].skill_lv);

			/** Delay: Remove the stock 5 secs and add half of the support time. **/
			mob_skills[i].delay += -5000 + (time1 + time2) / 2;

			if (mob_skills[i].delay < 2000)
				mob_skills[i].delay = 2000; /// With a minimum of 2 seconds.

			if (i + 1 < MAX_MOBSKILL) { /// Duplicate this so it also triggers on self.
				memcpy(&mob_skills[i + 1], &mob_skills[i], sizeof(struct mob_skill));
				db->maxskill = ++i;
				mob_skills[i].target = MST_SELF;
				mob_skills[i].cond1 = MSC_MYHPLTMAXRATE;
			}
		} else {
			switch (skill_id) { /// Certain special skills that are passive, and thus, never triggered.
			case MO_TRIPLEATTACK:
				FALLTHROUGH
			case TF_DOUBLE:
				FALLTHROUGH
			case GS_CHAINACTION:
				mob_skills[i].state = MSS_BERSERK;
				mob_skills[i].target = MST_TARGET;
				mob_skills[i].cond1 = MSC_ALWAYS;

				if (skill_id == MO_TRIPLEATTACK)
					mob_skills[i].permillage = 3000 - mob_skills[i].skill_lv * 100;
				else
					mob_skills[i].permillage = mob_skills[i].skill_lv * 500;

				mob_skills[i].delay -= 5000; /// Remove the added delay as these could trigger on "all hits".
				break;
			default: /// Untreated skill.
				continue;
			}
		}

		mob_skills[i].permillage = mob_skills[i].permillage * battle_config.mob_skill_rate / 100;
		mob_skills[i].delay = mob_skills[i].delay * battle_config.mob_skill_delay / 100;
		db->maxskill = ++i;
	}

	sd->fd = fd; /// We grant the session it's fd value back.

	/// Finally spawn it.
	struct mob_data *md = mob->once_spawn_sub(&sd->bl, m, x, y, DEFAULT_MOB_NAME, class_, event, SZ_SMALL, AI_NONE, 0);

	if (md == NULL)
		return 0; /// Failed?

	md->special_state.clone = 1;

	if (flag == 1) /// Friendly character.
		md->special_state.ai = AI_ATTACK;

	if (master_id != 0) /// Attach to master.
		md->master_id = master_id;

	if (duration > 0) { /// Auto delete after a while.
		if (md->deletetimer != INVALID_TIMER)
			timer->delete(md->deletetimer, mob->timer_delete);

		md->deletetimer = timer->add(timer->gettick() + duration, mob->timer_delete, md->bl.id, 0);
	}

	mob->spawn(md);
	return md->bl.id;
}

static int mob_clone_delete(struct mob_data *md)
{
	int class_;

	nullpo_ret(md);
	class_ = md->class_;
	if (class_ >= MOB_CLONE_START && class_ < MOB_CLONE_END && mob->db_data[class_] != NULL) {
		mob->destroy_mob_db(class_);
		//Clear references to the db
		md->db = mob->dummy;
		mob->free_dynamic_viewdata(md);
		md->vd = NULL;
		return 1;
	}
	return 0;
}

//
// Initialization
//
/*==========================================
 * Since un-setting [ mob ] up was used, it is an initial provisional value setup.
 *------------------------------------------*/
static int mob_makedummymobdb(int class_)
{
	if (mob->dummy != NULL)
	{
		if (mob->db(class_) == mob->dummy)
			return 1; //Using the mob->dummy data already. [Skotlex]
		if (class_ > 0 && class_ <= MAX_MOB_DB) {
			//Remove the mob data so that it uses the dummy data instead.
			mob->destroy_mob_db(class_);
		}
		return 0;
	}
	//Initialize dummy data.
	mob->dummy = (struct mob_db*)aCalloc(1, sizeof(struct mob_db)); //Initializing the dummy mob.
	sprintf(mob->dummy->sprite,"DUMMY");
	sprintf(mob->dummy->name,"Dummy");
	sprintf(mob->dummy->jname,"Dummy");
	mob->dummy->lv=1;
	mob->dummy->status.max_hp=1000;
	mob->dummy->status.max_sp=1;
	mob->dummy->status.rhw.range=1;
	mob->dummy->status.rhw.atk=7;
	mob->dummy->status.rhw.atk2=10;
	mob->dummy->status.str=1;
	mob->dummy->status.agi=1;
	mob->dummy->status.vit=1;
	mob->dummy->status.int_=1;
	mob->dummy->status.dex=6;
	mob->dummy->status.luk=2;
	mob->dummy->status.speed=300;
	mob->dummy->status.adelay=1000;
	mob->dummy->status.amotion=500;
	mob->dummy->status.dmotion=500;
	mob->dummy->base_exp=2;
	mob->dummy->job_exp=1;
	mob->dummy->range2=10;
	mob->dummy->range3=10;

	return 0;
}

//Adjusts the drop rate of item according to the criteria given. [Skotlex]
static unsigned int mob_drop_adjust(int baserate, int rate_adjust, unsigned short rate_min, unsigned short rate_max)
{
	int64 rate = baserate;

	Assert_ret(baserate >= 0);

	if (rate_adjust != 100 && baserate > 0) {
		if (battle_config.logarithmic_drops && rate_adjust > 0) {
			// Logarithmic drops equation by Ishizu-Chan
			//Equation: Droprate(x,y) = x * (5 - log(x)) ^ (ln(y) / ln(5))
			//x is the normal Droprate, y is the Modificator.
			rate = (int64)(baserate * pow((5.0 - log10(baserate)), (log(rate_adjust/100.) / log(5.0))) + 0.5);
		} else {
			//Classical linear rate adjustment.
			rate = apply_percentrate64(baserate, rate_adjust, 100);
		}
	}

	return (unsigned int)cap_value(rate,rate_min,rate_max);
}

static struct item_drop_ratio *mob_get_item_drop_ratio(int nameid)
{
	Assert_retr(NULL, nameid > 0);
	if (nameid < ARRAYLENGTH(item_drop_ratio_db)) {
		return item_drop_ratio_db[nameid];
	} else {
		return (struct item_drop_ratio *)idb_get(item_drop_ratio_other_db, nameid);
	}
}

static void mob_set_item_drop_ratio(int nameid, struct item_drop_ratio *ratio)
{
	Assert_retv(nameid > 0);
	if (nameid < ARRAYLENGTH(item_drop_ratio_db)) {
		Assert_retv(item_drop_ratio_db[nameid] == NULL);
		item_drop_ratio_db[nameid] = ratio;
	} else {
		Assert_retv(idb_get(item_drop_ratio_other_db, nameid) == NULL);
		idb_put(item_drop_ratio_other_db, nameid, ratio);
	}
}

/**
 * Check if global item drop rate is overridden for given item
 * in db/mob_item_ratio.txt
 * @param nameid ID of the item
 * @param mob_id ID of the monster
 * @param rate_adjust pointer to store ratio if found
 */
static void item_dropratio_adjust(int nameid, int mob_id, int *rate_adjust)
{
	struct item_drop_ratio *dropRatio;
	nullpo_retv(rate_adjust);

	dropRatio = mob->get_item_drop_ratio(nameid);
	if (dropRatio) {
		if (dropRatio->mob_id[0] ) { // only for listed mobs
			int i;
			ARR_FIND(0, MAX_ITEMRATIO_MOBS, i, dropRatio->mob_id[i] == mob_id);
			if (i < MAX_ITEMRATIO_MOBS) // found
				*rate_adjust = dropRatio->drop_ratio;
		}
		else // for all mobs
			*rate_adjust = dropRatio->drop_ratio;
	}
}

/* (mob_parse_dbrow)_cap_value */
static inline int mob_parse_dbrow_cap_value(int class_, int min, int max, int value)
{
	if( value > max ) {
		ShowError("mob_parse_dbrow_cap_value: for class '%d', field value '%d' is higher than the maximum '%d'! capping...\n", class_, value, max);
		return max;
	} else if ( value < min ) {
		ShowError("mob_parse_dbrow_cap_value: for class '%d', field value '%d' is lower than the minimum '%d'! capping...\n", class_, value, min);
		return min;
	}
	return value;
}

/**
 * Reads one possible option for a option slot in a option drop group
 * @param option : Libconfig entry
 * @param entry : memory db entry for current slot
 * @param idx : index of entry where this option should be inserted at
 * @param calc_rate : if rates should be recalculated after reading all entries
 * @param slot : option group slot being read (for messages)
 * @param group : option group being read (for messages)
 * @return true if it successfully read the entry, false otherwise
 */
static bool mob_read_optdrops_option(struct config_setting_t *option, struct optdrop_group_optslot *entry, int *idx, bool *calc_rate, int slot, const char *group)
{
	nullpo_retr(false, option);
	nullpo_retr(false, entry);
	nullpo_retr(false, idx);
	nullpo_retr(false, calc_rate);
	nullpo_retr(false, group);

	const char *name = config_setting_name(option);
	int opt_id;

	if (strncmp(name, "Rate", 4) == 0)
		return true;

	if (script->get_constant(name, &opt_id) == false) {
		ShowWarning("mob_read_optdrops_option: Invalid option \"%s\" for option slot %d of %s group, skipping.\n", name, slot, group);
		return false;
	}

	int min = 0, max = 0, opt_rate = 0;
	if (config_setting_is_number(option)) {
		// OptionName: value
		min = libconfig->setting_get_int(option);
	} else if (config_setting_is_array(option)) {
		// OptionName: [min, max]
		// OptionName: [min, max, rate]
		int slen = libconfig->setting_length(option);

		if (slen >= 2) {
			// [min, max,...]
			min = libconfig->setting_get_int_elem(option, 0);
			max = libconfig->setting_get_int_elem(option, 1);
		}

		if (slen == 3) {
			// [min, max, rate]
			opt_rate = libconfig->setting_get_int_elem(option, 2);
		}
	} else {
		ShowWarning("mob_read_optdrops_option: Invalid value \"%s\" for option slot %d of %s group, skipping.\n", name, slot, group);
		return false;
	}

	if (max < min)
		max = min;

	entry->options[*idx].id = opt_id;
	entry->options[*idx].min = min;
	entry->options[*idx].max = max;
	entry->options[*idx].rate = opt_rate;

	if (entry->options[*idx].rate == 0)
		*calc_rate = true;

	(*idx)++;

	return true;
}

/**
 * Reads the settings for one random option slot of a random option drop group.
 * @param optslot : The slot entry from config file
 * @param n : slot index
 * @param group_id : Group index
 * @param group : group name (used in messages)
 * @return true if it succesfully read, false otherwise
 */
static bool mob_read_optdrops_optslot(struct config_setting_t *optslot, int n, int group_id, const char *group)
{
	nullpo_retr(false, optslot);
	nullpo_retr(false, group);
	Assert_retr(false, group_id >= 0 && group_id < mob->opt_drop_groups_count);
	Assert_retr(false, n >= 0 && n < MAX_ITEM_OPTIONS);

	// Structure:
	//	{
	//		Rate: chance of option 1 (int)
	//		OptionName1: value
	//		OptionName2: [min, max]
	//		OptionName3: [min, max, rate]
	//      ....
	//	}

	int drop_rate; // The rate for this option to be dropped (Rate field)
	if (libconfig->setting_lookup_int(optslot, "Rate", &drop_rate) == CONFIG_FALSE) {
		ShowWarning("mob_read_optdrops_optslot: Missing option %d rate in group %s, skipping.\n", n, group);
		return false;
	}

	int count = libconfig->setting_length(optslot);
	if (count <= 1) { // 1 = Rate
		ShowWarning("mob_read_optdrops_optslot: Option %d of %s group doesn't contain any possible options, skipping.\n", n, group);
		return false;
	}

	struct optdrop_group_optslot *entry = &(mob->opt_drop_groups[group_id].optslot[n]);
	entry->options = aCalloc(count, sizeof(struct optdrop_group_option));

	int idx = 0;
	int i = 0;
	struct config_setting_t *opt = NULL;
	bool calc_rate = false;
	while (i < count && (opt = libconfig->setting_get_elem(optslot, i)) != NULL) {
		++i;
		mob->read_optdrops_option(opt, entry, &idx, &calc_rate, n, group);
	}
	entry->option_count = idx;
	mob->opt_drop_groups[group_id].optslot_count++;
	mob->opt_drop_groups[group_id].optslot_rate[n] = drop_rate;

	// If there're empty rates, calculate them
	if (calc_rate == true) {
		for (int j = 0; j < idx; ++j) {
			if (entry->options[j].rate == 0)
				entry->options[j].rate = 10000 / idx;
		}
	}

	return true;
}

/**
 * Reads one random option drop group.
 * @param group : Drop Group entry from config file
 * @param n : group index
 * @return true if it successfuly read, false otherwise
 */
static bool mob_read_optdrops_group(struct config_setting_t *group, int n)
{
	/* Structure:
	<Group Name>: (
		{ <Option 1 drop data> },
		{ <Option 2 drop data> },
		... // Up to MAX_ITEM_OPTIONS
	)
	*/
	nullpo_retr(false, group);

	const char *group_name = config_setting_name(group);

	if (group_name == NULL || *group_name == '\0') {
		ShowWarning("mob_read_optdrops_group: Invalid name for random option drop group, skipping group %d...\n", n);
		return false;
	}

	script->set_constant2(group_name, n, false, false);

	int i = 0;
	struct config_setting_t *drop_data = NULL;
	while (i < MAX_ITEM_OPTIONS && (drop_data = libconfig->setting_get_elem(group, i)) != NULL) {
		mob->read_optdrops_optslot(drop_data, i, n, group_name);
		i++;
	}

	return true;
}

/**
 * Reads random option drop groups database.
 */
static bool mob_read_optdrops_db(void)
{
	char filepath[256];
	libconfig->format_db_path("option_drop_groups.conf", filepath, sizeof(filepath));

	struct config_t option_groups;
	if (libconfig->load_file(&option_groups, filepath) == CONFIG_FALSE) {
		ShowError("Failed to load option drop groups\n");
		return false;
	}

	struct config_setting_t *its = libconfig->lookup(&option_groups, "option_drop_group_db");
	struct config_setting_t *groups = NULL;

	int i = 0;
	if (its != NULL && (groups = libconfig->setting_get_elem(its, 0)) != NULL) {
		int count = libconfig->setting_length(groups);
		mob->opt_drop_groups = aCalloc(count, sizeof(struct optdrop_group));
		mob->opt_drop_groups_count = count; // maximum size (used by assertions)

		struct config_setting_t *group = NULL;
		while ((group = libconfig->setting_get_elem(groups, i)) != NULL) {
			mob->read_optdrops_group(group, i);
			i++;
		}
		mob->opt_drop_groups_count = i; // number of entries used (should be the same amount)
	}

	libconfig->destroy(&option_groups);

	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", i, filepath);
	return true;
}

/**
 * Processes the stats for a mob database entry.
 *
 * @param[in,out] entry The destination mob_db entry, already initialized
 *                      (mob_id is expected to be already set).
 * @param[in]     t     The libconfig entry.
 */
static void mob_read_db_stats_sub(struct mob_db *entry, struct config_setting_t *t)
{
	int i32;
	nullpo_retv(entry);
	if (map->setting_lookup_const(t, "Str", &i32) && i32 >= 0) {
		entry->status.str = mob_parse_dbrow_cap_value(entry->mob_id, UINT16_MIN, UINT16_MAX, i32);
	}
	if (map->setting_lookup_const(t, "Agi", &i32) && i32 >= 0) {
		entry->status.agi = mob_parse_dbrow_cap_value(entry->mob_id, UINT16_MIN, UINT16_MAX, i32);
	}
	if (map->setting_lookup_const(t, "Vit", &i32) && i32 >= 0) {
		entry->status.vit = mob_parse_dbrow_cap_value(entry->mob_id, UINT16_MIN, UINT16_MAX, i32);
	}
	if (map->setting_lookup_const(t, "Int", &i32) && i32 >= 0) {
		entry->status.int_ = mob_parse_dbrow_cap_value(entry->mob_id, UINT16_MIN, UINT16_MAX, i32);
	}
	if (map->setting_lookup_const(t, "Dex", &i32) && i32 >= 0) {
		entry->status.dex = mob_parse_dbrow_cap_value(entry->mob_id, UINT16_MIN, UINT16_MAX, i32);
	}
	if (map->setting_lookup_const(t, "Luk", &i32) && i32 >= 0) {
		entry->status.luk = mob_parse_dbrow_cap_value(entry->mob_id, UINT16_MIN, UINT16_MAX, i32);
	}
}

/**
 * Processes the view data for a mob_db entry.
 *
 * @param[in,out] entry The destination mob_db entry, already initialized
 *                      (mob_id, status.mode are expected to be already set).
 * @param[in]     t     The libconfig entry.
 */
static void mob_read_db_viewdata_sub(struct mob_db *entry, struct config_setting_t *t)
{
	nullpo_retv(entry);
	nullpo_retv(t);

	struct config_setting_t *it;
	int i32;

	if ((it = libconfig->setting_get_member(t, "SpriteId")) != NULL)
		entry->vd.class = libconfig->setting_get_int(it);
	if ((it = libconfig->setting_get_member(t, "WeaponId")) != NULL)
		entry->vd.weapon = libconfig->setting_get_int(it);
	if ((it = libconfig->setting_get_member(t, "ShieldId")) != NULL)
		entry->vd.shield = libconfig->setting_get_int(it);
	if ((it = libconfig->setting_get_member(t, "RobeId")) != NULL)
		entry->vd.robe = libconfig->setting_get_int(it);
	if ((it = libconfig->setting_get_member(t, "HeadTopId")) != NULL)
		entry->vd.head_top = libconfig->setting_get_int(it);
	if ((it = libconfig->setting_get_member(t, "HeadMidId")) != NULL)
		entry->vd.head_mid = libconfig->setting_get_int(it);
	if ((it = libconfig->setting_get_member(t, "HeadLowId")) != NULL)
		entry->vd.head_bottom = libconfig->setting_get_int(it);

	if ((it = libconfig->setting_get_member(t, "HairStyleId")) != NULL)
		entry->vd.hair_style = libconfig->setting_get_int(it);
	else
		entry->vd.hair_style = 1;

	if ((it = libconfig->setting_get_member(t, "BodyStyleId")) != NULL)
		entry->vd.body_style = libconfig->setting_get_int(it);
	if ((it = libconfig->setting_get_member(t, "HairColorId")) != NULL)
		entry->vd.hair_color = libconfig->setting_get_uint16(it);
	if ((it = libconfig->setting_get_member(t, "BodyColorId")) != NULL)
		entry->vd.cloth_color = libconfig->setting_get_uint16(it);
	if (map->setting_lookup_const(t, "Gender", &i32) && i32 >= 0) {
		entry->vd.sex = (char)i32;
	}
	if ((it = libconfig->setting_get_member(t, "Options")) != NULL)
		entry->option = libconfig->setting_get_int(it) &~ (OPTION_HIDE | OPTION_CLOAK | OPTION_INVISIBLE);
}

/**
 * Processes the mode for a mob_db entry.
 *
 * @param[in] entry The destination mob_db entry, already initialized.
 * @param[in] t     The libconfig entry.
 *
 * @return The parsed mode.
 */
static uint32 mob_read_db_mode_sub(struct mob_db *entry, struct config_setting_t *t)
{
	uint32 mode = 0;
	struct config_setting_t *t2;

	if ((t2 = libconfig->setting_get_member(t, "CanMove")))
		mode |= libconfig->setting_get_bool(t2) ? MD_CANMOVE : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "Looter")))
		mode |= libconfig->setting_get_bool(t2) ? MD_LOOTER : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "Aggressive")))
		mode |= libconfig->setting_get_bool(t2) ? MD_AGGRESSIVE : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "Assist")))
		mode |= libconfig->setting_get_bool(t2) ? MD_ASSIST : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "CastSensorIdle")))
		mode |= libconfig->setting_get_bool(t2) ? MD_CASTSENSOR_IDLE : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "Boss")))
		mode |= libconfig->setting_get_bool(t2) ? MD_BOSS : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "Plant")))
		mode |= libconfig->setting_get_bool(t2) ? MD_PLANT : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "CanAttack")))
		mode |= libconfig->setting_get_bool(t2) ? MD_CANATTACK : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "Detector")))
		mode |= libconfig->setting_get_bool(t2) ? MD_DETECTOR : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "CastSensorChase")))
		mode |= libconfig->setting_get_bool(t2) ? MD_CASTSENSOR_CHASE : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "ChangeChase")))
		mode |= libconfig->setting_get_bool(t2) ? MD_CHANGECHASE : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "Angry")))
		mode |= libconfig->setting_get_bool(t2) ? MD_ANGRY : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "ChangeTargetMelee")))
		mode |= libconfig->setting_get_bool(t2) ? MD_CHANGETARGET_MELEE : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "ChangeTargetChase")))
		mode |= libconfig->setting_get_bool(t2) ? MD_CHANGETARGET_CHASE : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "TargetWeak")))
		mode |= libconfig->setting_get_bool(t2) ? MD_TARGETWEAK : MD_NONE;
	if ((t2 = libconfig->setting_get_member(t, "NoKnockback")))
		mode |= libconfig->setting_get_bool(t2) ? MD_NOKNOCKBACK : MD_NONE;

	return mode & MD_MASK;
}

/**
 * Process an entry of mob/mvp drops that contains a random option drop group.
 *
 * @param entry : mob db entry being read (used in error messages)
 * @param item_name : AegisName of the item in this entry (used in error messages)
 * @param drop : drop data entry
 * @param drop_rate : used to return the entry drop_rate
 * @returns a reference to the opt_drop_group to be used when creating this item drop
 */
static struct optdrop_group *mob_read_db_drops_option(struct mob_db *entry, const char *item_name, struct config_setting_t *drop, int *drop_rate)
{
	nullpo_retr(NULL, entry);
	nullpo_retr(NULL, item_name);
	nullpo_retr(NULL, drop);
	nullpo_retr(NULL, drop_rate);

	// (Drop Rate, "Option Group")
	if (!config_setting_is_list(drop) || config_setting_length(drop) != 2) {
		ShowError("mob_read_db_optdrops: Invalid format for option drop group on item \"%s\" in monster %d, skipping.\n", item_name, entry->mob_id);
		return NULL;
	}

	int i32;
	if (mob->get_const(libconfig->setting_get_elem(drop, 0), &i32) && i32 >= 0)
		*drop_rate = i32;

	const char *group_name = libconfig->setting_get_string_elem(drop, 1);
	if (group_name == NULL || *group_name == '\0') {
		ShowError("mob_read_db_optdrops: Missing option drop group name on item \"%s\" in monster %d, skipping.\n", item_name, entry->mob_id);
		return NULL;
	}

	int opt_id;
	if (script->get_constant(group_name, &opt_id) == false) {
		ShowError("mob_read_db_optdrops: Invalid option drop group \"%s\" on item \"%s\" in monster %d, does this group really exists? Skipping...\n", group_name, item_name, entry->mob_id);
		return NULL;
	}
	if (opt_id < 0 || opt_id >= mob->opt_drop_groups_count) {
		ShowError("mob_read_db_optdrops: Invalid option drop group \"%s\" index \"%d\" on item \"%s\" in monster %d, does this group really exists? Skipping...\n", group_name, opt_id, item_name, entry->mob_id);
		return NULL;
	}

	return &mob->opt_drop_groups[opt_id];
}

/**
 * Processes the MVP drops for a mob_db entry.
 *
 * @param[in,out] entry The destination mob_db entry, already initialized
 *                      (mob_id is expected to be already set).
 * @param[in]     t     The libconfig entry.
 */
static void mob_read_db_mvpdrops_sub(struct mob_db *entry, struct config_setting_t *t)
{
	struct config_setting_t *drop;
	int i = 0;
	int idx = 0;
	int i32;

	nullpo_retv(entry);
	while (idx < MAX_MVP_DROP && (drop = libconfig->setting_get_elem(t, i))) {
		const char *name = config_setting_name(drop);
		int rate_adjust = battle_config.item_rate_mvp;
		struct item_data* id = itemdb->search_name(name);
		int value = 0;
		if (!id) {
			ShowWarning("mob_read_db: mvp drop item %s not found in monster %d\n", name, entry->mob_id);
			i++;
			continue;
		}

		struct optdrop_group *drop_option = NULL;
		if (config_setting_is_number(drop)) {
			// Setting is a number, item doesn't contain options
			if (mob->get_const(drop, &i32) && i32 >= 0) {
				value = i32;
			}
		} else {
			// (Drop Rate, "Opt Drop Group")
			drop_option = mob->read_db_drops_option(entry, name, drop, &value);
		}

		if (value <= 0) {
			ShowWarning("mob_read_db: wrong drop chance %d for mvp drop item %s in monster %d\n", value, name, entry->mob_id);
			i++;
			continue;
		}
		entry->mvpitem[idx].nameid = id->nameid;
		if (!entry->mvpitem[idx].nameid) {
			entry->mvpitem[idx].p = 0; //No item....
			i++;
			continue;
		}
		mob->item_dropratio_adjust(entry->mvpitem[idx].nameid, entry->mob_id, &rate_adjust);
		entry->mvpitem[idx].p = mob->drop_adjust(value, rate_adjust, battle_config.item_drop_mvp_min, battle_config.item_drop_mvp_max);
		entry->mvpitem[idx].options = drop_option;

		//calculate and store Max available drop chance of the MVP item
		if (entry->mvpitem[idx].p) {
			if (id->maxchance == -1 || (id->maxchance < entry->mvpitem[idx].p/10 + 1) ) {
				//item has bigger drop chance or sold in shops
				id->maxchance = entry->mvpitem[idx].p/10 + 1; //reduce MVP drop info to not spoil common drop rate
			}
		}
		i++;
		idx++;
	}
	if (idx == MAX_MVP_DROP && libconfig->setting_get_elem(t, i)) {
		ShowWarning("mob_read_db: Too many mvp drops in mob %d\n", entry->mob_id);
	}
}

/**
 * Processes the drops for a mob_db entry.
 *
 * @param[in,out] entry The destination mob_db entry, already initialized
 *                      (mob_id, status.mode are expected to be already set).
 * @param[in]     t     The libconfig entry.
 */
static void mob_read_db_drops_sub(struct mob_db *entry, struct config_setting_t *t)
{
	struct config_setting_t *drop;
	int i = 0;
	int idx = 0;
	int i32;
	int k;

	nullpo_retv(entry);
	while (idx < MAX_MOB_DROP && (drop = libconfig->setting_get_elem(t, i))) {
		const char *name = config_setting_name(drop);
		int rate_adjust, type;
		unsigned short ratemin, ratemax;
		struct item_data* id = itemdb->search_name(name);
		int value = 0;
		if (!id) {
			ShowWarning("mob_read_db: drop item %s not found in monster %d\n", name, entry->mob_id);
			i++;
			continue;
		}

		struct optdrop_group *drop_option = NULL;
		if (config_setting_is_number(drop)) {
			// Setting is a number, item doesn't contain options
			if (mob->get_const(drop, &i32) && i32 >= 0) {
				value = i32;
			}
		} else {
			// (Drop Rate, "Opt Drop Group")
			drop_option = mob->read_db_drops_option(entry, name, drop, &value);
		}

		if (value <= 0) {
			ShowWarning("mob_read_db: wrong drop chance %d for drop item %s in monster %d\n", value, name, entry->mob_id);
			i++;
			continue;
		}

		entry->dropitem[idx].nameid = id->nameid;
		entry->dropitem[idx].options = drop_option;
		if (!entry->dropitem[idx].nameid) {
			entry->dropitem[idx].p = 0; //No drop.
			i++;
			continue;
		}
		type = id->type;
		if ((entry->mob_id >= MOBID_TREASURE_BOX1 && entry->mob_id <= MOBID_TREASURE_BOX40)
		 || (entry->mob_id >= MOBID_TREASURE_BOX41 && entry->mob_id <= MOBID_TREASURE_BOX49)) {
			//Treasure box drop rates [Skotlex]
			rate_adjust = battle_config.item_rate_treasure;
			ratemin = battle_config.item_drop_treasure_min;
			ratemax = battle_config.item_drop_treasure_max;
		} else {
			switch (type) { // Added support to restrict normal drops of MVP's [Reddozen]
			case IT_HEALING:
				rate_adjust = (entry->status.mode&MD_BOSS) ? battle_config.item_rate_heal_boss : battle_config.item_rate_heal;
				ratemin = battle_config.item_drop_heal_min;
				ratemax = battle_config.item_drop_heal_max;
				break;
			case IT_USABLE:
			case IT_CASH:
				rate_adjust = (entry->status.mode&MD_BOSS) ? battle_config.item_rate_use_boss : battle_config.item_rate_use;
				ratemin = battle_config.item_drop_use_min;
				ratemax = battle_config.item_drop_use_max;
				break;
			case IT_WEAPON:
			case IT_ARMOR:
			case IT_PETARMOR:
				rate_adjust = (entry->status.mode&MD_BOSS) ? battle_config.item_rate_equip_boss : battle_config.item_rate_equip;
				ratemin = battle_config.item_drop_equip_min;
				ratemax = battle_config.item_drop_equip_max;
				break;
			case IT_CARD:
				rate_adjust = (entry->status.mode&MD_BOSS) ? battle_config.item_rate_card_boss : battle_config.item_rate_card;
				ratemin = battle_config.item_drop_card_min;
				ratemax = battle_config.item_drop_card_max;
				break;
			default:
				rate_adjust = (entry->status.mode&MD_BOSS) ? battle_config.item_rate_common_boss : battle_config.item_rate_common;
				ratemin = battle_config.item_drop_common_min;
				ratemax = battle_config.item_drop_common_max;
				break;
			}
		}
		mob->item_dropratio_adjust(id->nameid, entry->mob_id, &rate_adjust);
		entry->dropitem[idx].p = mob->drop_adjust(value, rate_adjust, ratemin, ratemax);

		//calculate and store Max available drop chance of the item
		if (entry->dropitem[idx].p
		 && (entry->mob_id < MOBID_TREASURE_BOX1 || entry->mob_id > MOBID_TREASURE_BOX40)
		 && (entry->mob_id < MOBID_TREASURE_BOX41 || entry->mob_id > MOBID_TREASURE_BOX49)) {
			//Skip treasure chests.
			if (id->maxchance == -1 || (id->maxchance < entry->dropitem[idx].p) ) {
				id->maxchance = entry->dropitem[idx].p; //item has bigger drop chance or sold in shops
			}
			for (k = 0; k< MAX_SEARCH; k++) {
				if (id->mob[k].chance <= entry->dropitem[idx].p)
					break;
			}
			if (k == MAX_SEARCH) {
				i++;
				idx++;
				continue;
			}

			if (id->mob[k].id != entry->mob_id && k != MAX_SEARCH - 1)
				memmove(&id->mob[k+1], &id->mob[k], (MAX_SEARCH-k-1)*sizeof(id->mob[0]));
			id->mob[k].chance = entry->dropitem[idx].p;
			id->mob[k].id = entry->mob_id;
		}
		i++;
		idx++;
	}
	if (idx == MAX_MOB_DROP && libconfig->setting_get_elem(t, i)) {
		ShowWarning("mob_read_db: Too many drops in mob %d\n", entry->mob_id);
	}
}

/**
 * Validates a mob DB entry and inserts it into the database.
 * This function is called after preparing the mob entry data, and it takes
 * care of inserting it and cleaning up any remainders of the previous one (in
 * case it is overwriting an existing entry).
 *
 * @param entry  Pointer to the new mob_db entry. Ownership is NOT taken, but
 *               the content is modified to reflect the validation.
 * @param n      Ordinal number of the entry, to be displayed in case of
 *               validation errors.
 * @param source Source of the entry (file name), to be displayed in case of
 *               validation errors.
 * @return Mob ID of the validated entry, or 0 in case of failure.
 *
 * Note: This is safe to call if the new entry is a shallow copy of the old one
 * (i.e.  mob_db2 inheritance), as it will make sure not to free any data still
 * in use by the new entry.
 */
static int mob_db_validate_entry(struct mob_db *entry, int n, const char *source)
{
	struct mob_data data;

	nullpo_ret(entry);
	if (entry->mob_id <= 1000 || entry->mob_id > MAX_MOB_DB) {
		ShowError("mob_db_validate_entry: Invalid monster ID %d, must be in range %d-%d.\n", entry->mob_id, 1000, MAX_MOB_DB);
		return 0;
	}
	if (pc->db_checkid(entry->mob_id)) {
		ShowError("mob_read_db_sub: Invalid monster ID %d, reserved for player classes.\n", entry->mob_id);
		return 0;
	}
	if (entry->mob_id >= MOB_CLONE_START && entry->mob_id < MOB_CLONE_END) {
		ShowError("mob_read_db_sub: Invalid monster ID %d. Range %d-%d is reserved for player clones. Please increase MAX_MOB_DB (%d).\n",
				entry->mob_id, MOB_CLONE_START, MOB_CLONE_END-1, MAX_MOB_DB);
		return 0;
	}

	entry->lv = cap_value(entry->lv, 1, USHRT_MAX);

	if (entry->status.max_sp < 1)
		entry->status.max_sp = 1;
	//Since mobs always respawn with full life...
	entry->status.hp = entry->status.max_hp;
	entry->status.sp = entry->status.max_sp;

	/*
	 * Disabled for renewal since difference of 0 and 1 still has an impact in the formulas
	 * Just in case there is a mishandled division by zero please let us know. [malufett]
	 */
#ifndef RENEWAL
	//All status should be min 1 to prevent divisions by zero from some skills. [Skotlex]
	if (entry->status.str < 1) entry->status.str = 1;
	if (entry->status.agi < 1) entry->status.agi = 1;
	if (entry->status.vit < 1) entry->status.vit = 1;
	if (entry->status.int_< 1) entry->status.int_= 1;
	if (entry->status.dex < 1) entry->status.dex = 1;
	if (entry->status.luk < 1) entry->status.luk = 1;
#endif

	if (entry->range2 < 1)
		entry->range2 = 1;

#if 0 // This code was (accidentally) never enabled. It'll stay commented out until it's proven to be needed.
	//Tests showed that chase range is effectively 2 cells larger than expected [Playtester]
	if (entry->range3 > 0)
		entry->range3 += 2;
#endif // 0

	if (entry->range3 < entry->range2)
		entry->range3 = entry->range2;

	entry->status.size = cap_value(entry->status.size, 0, 2);

	entry->status.race = cap_value(entry->status.race, 0, RC_MAX - 1);

	if (entry->status.def_ele >= ELE_MAX) {
		ShowWarning("mob_read_db_sub: Invalid element type %d for monster ID %d (max=%d).\n", entry->status.def_ele, entry->mob_id, ELE_MAX-1);
		entry->status.def_ele = ELE_NEUTRAL;
		entry->status.ele_lv = 1;
	}
	if (entry->status.ele_lv < 1 || entry->status.ele_lv > 4) {
		ShowWarning("mob_read_db_sub: Invalid element level %d for monster ID %d, must be in range 1-4.\n", entry->status.ele_lv, entry->mob_id);
		entry->status.ele_lv = 1;
	}

	// If the attack animation is longer than the delay, the client crops the attack animation!
	// On aegis there is no real visible effect of having a recharge-time less than amotion anyway.
	if (entry->status.adelay < entry->status.amotion)
		entry->status.adelay = entry->status.amotion;

	// Fill in remaining status data by using a dummy monster.
	data.bl.type = BL_MOB;
	data.level = entry->lv;
	memcpy(&data.status, &entry->status, sizeof(struct status_data));
	status->calc_misc(&data.bl, &entry->status, entry->lv);

	// Finally insert monster's data into the database.
	if (mob->db_data[entry->mob_id] == NULL) {
		mob->db_data[entry->mob_id] = (struct mob_db*)aMalloc(sizeof(struct mob_db));
	} else {
		//Copy over spawn data
		memcpy(&entry->spawn, mob->db_data[entry->mob_id]->spawn, sizeof(entry->spawn));
	}
	memcpy(mob->db_data[entry->mob_id], entry, sizeof(struct mob_db));

	return entry->mob_id;
}

/**
 * Processes one mobdb entry from the libconfig file, loading and inserting it
 * into the mob database.
 *
 * @param mobt   Libconfig setting entry. It is expected to be valid and it
 *               won't be freed (it is care of the caller to do so if
 *               necessary).
 * @param n      Ordinal number of the entry, to be displayed in case of
 *               validation errors.
 * @param source Source of the entry (file name), to be displayed in case of
 *               validation errors.
 * @return Mob ID of the validated entry, or 0 in case of failure.
 */
static int mob_read_db_sub(struct config_setting_t *mobt, int n, const char *source)
{
	struct mob_db md = { 0 };
	struct config_setting_t *t = NULL;
	const char *str = NULL;
	int i32 = 0;
	bool inherit = false;
	bool maxhpUpdated = false;

	nullpo_ret(mobt);
	/*
	 * // Mandatory fields
	 * Id: ID
	 * SpriteName: "SPRITE_NAME"
	 * Name: "Mob name"
	 * JName: "Mob name"
	 * // Optional fields
	 * Lv: level
	 * Hp: health
	 * Sp: mana
	 * Exp: basic experience
	 * JExp: job experience
	 * AttackRange: attack range
	 * Attack: [attack1, attack2]
	 * Def: defence
	 * Mdef: magic defence
	 * Stats: {
	 *     Str: strength
	 *     Agi: agility
	 *     Vit: vitality
	 *     Int: intelligence
	 *     Dex: dexterity
	 *     Luk: luck
	 * }
	 * ViewRange: view range
	 * ChaseRange: chase range
	 * Size: size
	 * Race: race
	 * Element: (type, level)
	 * Mode: {
	 *     CanMove: true/false
	 *     Looter: true/false
	 *     Aggressive: true/false
	 *     Assist: true/false
	 *     CastSensorIdle:true/false
	 *     Boss: true/false
	 *     Plant: true/false
	 *     CanAttack: true/false
	 *     Detector: true/false
	 *     CastSensorChase: true/false
	 *     ChangeChase: true/false
	 *     Angry: true/false
	 *     ChangeTargetMelee: true/false
	 *     ChangeTargetChase: true/false
	 *     TargetWeak: true/false
	 *     NoKnockback: true/false
	 * }
	 * MoveSpeed: move speed
	 * AttackDelay: attack delay
	 * AttackMotion: attack motion
	 * DamageMotion: damage motion
	 * MvpExp: mvp experience
	 * DamageTakenRate: damage taken rate
	 * MvpDrops: {
	 *     AegisName: chance
	 *     ...
	 * }
	 * Drops: {
	 *     AegisName: chance
	 *     // or
	 *     AegisName: (chance, "Option Drop Group")
	 *     ...
	 * }
	 * DamageTakenRate: damage taken rate
	 * ViewData: {
	 *     SpriteId: sprite id
	 *     WeaponId: weapon id
	 *     ShieldId: shield id
	 *     RobeId: garment id
	 *     HeadTopId: top headgear id
	 *     HeadMidId: middle headgear id
	 *     HeadLowId: lower headgear id
	 *     HairStyleId: hair style id
	 *     BodyStyleId: clothes id
	 *     HairColorId: hair color id
	 *     BodyColorId: clothes color id
	 *     Gender: gender
	 *     Options: options
	 * }
	 */

	if (!libconfig->setting_lookup_int(mobt, "Id", &i32)) {
		ShowWarning("mob_read_db_sub: Missing id in \"%s\", entry #%d, skipping.\n", source, n);
		return 0;
	}
	md.mob_id = i32;
	md.vd.class = md.mob_id;

	if ((t = libconfig->setting_get_member(mobt, "Inherit")) && (inherit = libconfig->setting_get_bool(t))) {
		if (!mob->db_data[md.mob_id]) {
			ShowWarning("mob_read_db_sub: Trying to inherit nonexistent mob %d, default values will be used instead.\n", md.mob_id);
			inherit = false;
		} else {
			// Use old entry as default
			struct mob_db *old_entry = mob->db_data[md.mob_id];
			memcpy(&md, old_entry, sizeof(md));
		}
	}

	if (!libconfig->setting_lookup_string(mobt, "SpriteName", &str) || !*str ) {
		if (!inherit) {
			ShowWarning("mob_read_db_sub: Missing SpriteName in mob %d of \"%s\", skipping.\n", md.mob_id, source);
			return 0;
		}
	} else {
		safestrncpy(md.sprite, str, sizeof(md.sprite));
	}

	if (!libconfig->setting_lookup_string(mobt, "Name", &str) || !*str ) {
		if (!inherit) {
			ShowWarning("mob_read_db_sub: Missing Name in mob %d of \"%s\", skipping.\n", md.mob_id, source);
			return 0;
		}
	} else {
		safestrncpy(md.name, str, sizeof(md.name));
	}

	if (!libconfig->setting_lookup_string(mobt, "JName", &str) || !*str ) {
		if (!inherit) {
			safestrncpy(md.jname, md.name, sizeof(md.jname));
		}
	} else {
		safestrncpy(md.jname, str, sizeof(md.jname));
	}

	if (map->setting_lookup_const(mobt, "Lv", &i32) && i32 >= 0) {
		md.lv = i32;
	} else if (!inherit) {
		md.lv = 1;
	}

	if (map->setting_lookup_const(mobt, "Hp", &i32) && i32 >= 0) {
		md.status.max_hp = i32;
		maxhpUpdated = true; // battle_config modifiers to max_hp are applied below
	} else if (!inherit) {
		md.status.max_hp = 1;
		maxhpUpdated = true; // battle_config modifiers to max_hp are applied below
	}

	if (map->setting_lookup_const(mobt, "Sp", &i32) && i32 >= 0) {
		md.status.max_sp = i32;
	} else if (!inherit) {
		md.status.max_sp = 1;
	}

	if (map->setting_lookup_const(mobt, "Exp", &i32) && i32 >= 0) {
		int64 exp = apply_percentrate64(i32, battle_config.base_exp_rate, 100);
		md.base_exp = (unsigned int)cap_value(exp, 0, UINT_MAX);
	}

	if (map->setting_lookup_const(mobt, "JExp", &i32) && i32 >= 0) {
		int64 exp = apply_percentrate64(i32, battle_config.job_exp_rate, 100);
		md.job_exp = (unsigned int)cap_value(exp, 0, UINT_MAX);
	}

	if (map->setting_lookup_const(mobt, "AttackRange", &i32) && i32 >= 0) {
		md.status.rhw.range = i32;
	} else if (!inherit) {
		md.status.rhw.range = 1;
	}

	if ((t = libconfig->setting_get_member(mobt, "Attack"))) {
		if (config_setting_is_aggregate(t)) {
			if (libconfig->setting_length(t) >= 2)
				md.status.rhw.atk2 = libconfig->setting_get_int_elem(t, 1);
			if (libconfig->setting_length(t) >= 1)
				md.status.rhw.atk = libconfig->setting_get_int_elem(t, 0);
		} else if (map->setting_lookup_const(mobt, "Attack", &i32) && i32 >= 0) {
			md.status.rhw.atk = i32;
			md.status.rhw.atk2 = i32;
		}
	}

	if (map->setting_lookup_const(mobt, "Def", &i32) && i32 >= 0) {
		md.status.def = mob_parse_dbrow_cap_value(md.mob_id, DEFTYPE_MIN, DEFTYPE_MAX, i32);
	}
	if (map->setting_lookup_const(mobt, "Mdef", &i32) && i32 >= 0) {
		md.status.mdef = mob_parse_dbrow_cap_value(md.mob_id, DEFTYPE_MIN, DEFTYPE_MAX, i32);
	}

	if ((t = libconfig->setting_get_member(mobt, "Stats"))) {
		if (config_setting_is_group(t)) {
			mob->read_db_stats_sub(&md, t);
		} else if (map->setting_lookup_const(mobt, "Stats", &i32) && i32 >= 0) {
			md.status.str = md.status.agi = md.status.vit = md.status.int_ = md.status.dex = md.status.luk =
				mob_parse_dbrow_cap_value(md.mob_id, UINT16_MIN, UINT16_MAX, i32);
		}
	}

	if (map->setting_lookup_const(mobt, "ViewRange", &i32) && i32 >= 0) {
		if (battle_config.view_range_rate != 100) {
			md.range2 = i32 * battle_config.view_range_rate / 100;
		} else {
			md.range2 = i32;
		}
	} else if (!inherit) {
		md.range2 = 1;
	}

	if (map->setting_lookup_const(mobt, "ChaseRange", &i32) && i32 >= 0) {
		if (battle_config.chase_range_rate != 100) {
			md.range3 = i32 * battle_config.chase_range_rate / 100;
		} else {
			md.range3 = i32;
		}
	} else if (!inherit) {
		md.range3 = 1;
	}

	if (map->setting_lookup_const(mobt, "Size", &i32) && i32 >= 0) {
		md.status.size = i32;
	} else if (!inherit) {
		md.status.size = 0;
	}

	if (map->setting_lookup_const(mobt, "Race", &i32) && i32 >= 0) {
		md.status.race = i32;
	} else if (!inherit) {
		md.status.race = 0;
	}

	if ((t = libconfig->setting_get_member(mobt, "Element")) && config_setting_is_list(t)) {
		int value = 0;
		if (mob->get_const(libconfig->setting_get_elem(t, 0), &i32) && mob->get_const(libconfig->setting_get_elem(t, 1), &value)) {
			md.status.def_ele = i32;
			md.status.ele_lv = value;
		} else if (!inherit) {
			md.status.def_ele = ELE_NEUTRAL;
			md.status.ele_lv = 1;
		}
	} else if (!inherit) {
		md.status.def_ele = ELE_NEUTRAL;
		md.status.ele_lv = 1;
	}

	if ((t = libconfig->setting_get_member(mobt, "Mode"))) {
		if (config_setting_is_group(t)) {
			md.status.mode = mob->read_db_mode_sub(&md, t);
		} else if (map->setting_lookup_const(mobt, "Mode", &i32) && i32 >= 0) {
			md.status.mode = (uint32)i32 & MD_MASK;
		}
	}
	if (!battle_config.monster_active_enable)
		md.status.mode &= ~MD_AGGRESSIVE;

	if (map->setting_lookup_const(mobt, "MoveSpeed", &i32) && i32 >= 0) {
		md.status.speed = i32;
	}

	md.status.aspd_rate = 1000;

	if (map->setting_lookup_const(mobt, "AttackDelay", &i32) && i32 >= 0) {
		md.status.adelay = cap_value(i32, battle_config.monster_max_aspd*2, 4000);
	} else if (!inherit) {
		md.status.adelay = 4000;
	}

	if (map->setting_lookup_const(mobt, "AttackMotion", &i32) && i32 >= 0) {
		md.status.amotion = cap_value(i32, battle_config.monster_max_aspd, 2000);
	} else if (!inherit) {
		md.status.amotion = 2000;
	}

	if (map->setting_lookup_const(mobt, "DamageMotion", &i32) && i32 >= 0) {
		if (battle_config.monster_damage_delay_rate != 100)
			md.status.dmotion = i32 * battle_config.monster_damage_delay_rate / 100;
		else
			md.status.dmotion = i32;
	}

	// MVP EXP Bonus: MEXP
	if (map->setting_lookup_const(mobt, "MvpExp", &i32) && i32 >= 0) {
		// Some new MVP's MEXP multiple by high exp-rate cause overflow. [LuzZza]
		int64 exp = apply_percentrate64(i32, battle_config.mvp_exp_rate, 100);
		md.mexp = (unsigned int)cap_value(exp, 0, UINT_MAX);
	}

	if (maxhpUpdated) {
		// Now that we know if it is an mvp or not, apply battle_config modifiers [Skotlex]
		int64 maxhp = md.status.max_hp;
		if (md.mexp > 0) { //Mvp
			maxhp = apply_percentrate64(maxhp, battle_config.mvp_hp_rate, 100);
		} else { //Normal mob
			maxhp = apply_percentrate64(maxhp, battle_config.monster_hp_rate, 100);
		}
		md.status.max_hp = (unsigned int)cap_value(maxhp, 1, UINT_MAX);
	}

	if ((t = libconfig->setting_get_member(mobt, "MvpDrops"))) {
		if (config_setting_is_group(t)) {
			mob->read_db_mvpdrops_sub(&md, t);
		}
	}

	if ((t = libconfig->setting_get_member(mobt, "Drops"))) {
		if (config_setting_is_group(t)) {
			mob->read_db_drops_sub(&md, t);
		}
	}

	if (map->setting_lookup_const(mobt, "DamageTakenRate", &i32) && i32 >= 0) {
		md.dmg_taken_rate = cap_value(i32, 1, INT_MAX);
	} else if (!inherit) {
		md.dmg_taken_rate = 100;
	}

	if ((t = libconfig->setting_get_member(mobt, "ViewData"))) {
		if (config_setting_is_group(t)) {
			mob->read_db_viewdata_sub(&md, t);
		}
	}

	mob->read_db_additional_fields(&md, mobt, n, source);

	return mob->db_validate_entry(&md, n, source);
}

/**
 * Processes any (plugin-defined) additional fields for a mob_db entry.
 *
 * @param[in,out] entry  The destination mob_db entry, already initialized
 *                       (mob_id, status.mode are expected to be already set).
 * @param[in]     t      The libconfig entry.
 * @param[in]     n      Ordinal number of the entry, to be displayed in case
 *                       of validation errors.
 * @param[in]     source Source of the entry (file name), to be displayed in
 *                       case of validation errors.
 */
static void mob_read_db_additional_fields(struct mob_db *entry, struct config_setting_t *t, int n, const char *source)
{
	// do nothing. plugins can do own work
}

static bool mob_get_const(const struct config_setting_t *it, int *value)
{
	const char *str = config_setting_get_string(it);

	nullpo_retr(false, value);
	if (str && *str && script->get_constant(str, value))
		return true;

	*value = libconfig->setting_get_int(it);
	return true;
}

/*==========================================
 * mob_db.txt reading
 *------------------------------------------*/
static void mob_readdb(void)
{
	const char* filename[] = {
		DBPATH"mob_db.conf",
		"mob_db2.conf" };
	int i;

	for (i = 0; i < ARRAYLENGTH(filename); ++i) {
		mob->read_libconfig(filename[i], i > 0 ? true : false);
	}
	mob->name_constants();
}

/**
 * Reads from a libconfig-formatted mobdb file and inserts the found entries
 * into the mob database, overwriting duplicate ones (i.e. mob_db2 overriding
 * mob_db.)
 *
 * @param filename       File name, relative to the database path.
 * @param ignore_missing Whether to ignore errors caused by a missing db file.
 * @return the number of found entries.
 */
static int mob_read_libconfig(const char *filename, bool ignore_missing)
{
	bool duplicate[MAX_MOB_DB] = { 0 };
	struct config_t mob_db_conf;
	char filepath[512];
	struct config_setting_t *mdb;
	struct config_setting_t *t;
	int i = 0, count = 0;

	nullpo_ret(filename);
	snprintf(filepath, sizeof(filepath), "%s/%s", map->db_path, filename);

	if (ignore_missing && !exists(filepath))
		return 0;

	if (!libconfig->load_file(&mob_db_conf, filepath))
		return 0;

	if ((mdb = libconfig->setting_get_member(mob_db_conf.root, "mob_db")) == NULL) {
		ShowError("can't read %s\n", filepath);
		return 0;
	}

	while ((t = libconfig->setting_get_elem(mdb, i++))) {
		int mob_id = mob->read_db_sub(t, i - 1, filename);

		if (mob_id <= 0 || mob_id >= MAX_MOB_DB)
			continue;

		count++;

		if (duplicate[mob_id]) {
			ShowWarning("mob_read_libconfig:%s: duplicate entry of ID #%d (%s/%s)\n",
					filename, mob_id, mob->db_data[mob_id]->sprite, mob->db_data[mob_id]->jname);
		} else {
			duplicate[mob_id] = true;
		}
	}
	libconfig->destroy(&mob_db_conf);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", count, filepath);

	return count;
}

static void mob_name_constants(void)
{
	int i;
#ifdef ENABLE_CASE_CHECK
	script->parser_current_file = "Mob Database (Likely an invalid or conflicting SpriteName)";
#endif // ENABLE_CASE_CHECK
	for (i = 0; i < MAX_MOB_DB; i++) {
		if (mob->db_data[i] && !mob->is_clone(i))
			script->set_constant2(mob->db_data[i]->sprite, i, false, false);
	}
#ifdef ENABLE_CASE_CHECK
	script->parser_current_file = NULL;
#endif // ENABLE_CASE_CHECK
}

static void mob_mobavail_removal_notice(void)
{
	char filepath[270];

	snprintf(filepath, sizeof(filepath), "%s/mob_avail.txt", map->db_path);

	if (exists(filepath)) {
		ShowError("mob_mobavail_removal_notice: the usage of mob_avail.txt is no longer supported, move your data using tools/mobavailconverter.py and delete the database file to suspend this message.\n");
	}
}

static void mob_read_group_db(void)
{
	const char *filename[] = {
		DBPATH"mob_group.conf",
		"mob_group2.conf"
	};

	for (int i = 0; i < ARRAYLENGTH(filename); i++)
		mob->read_group_db_libconfig(filename[i]);
}

static bool mob_read_group_db_libconfig(const char *filename)
{
	struct config_t mg_conf;
	char filepath[512];

	snprintf(filepath, sizeof(filepath), "%s/%s", map->db_path, filename);
	if (libconfig->load_file(&mg_conf, filepath) == CONFIG_FALSE) {
		ShowError("%s: can't read %s\n", __func__, filepath);
		return false;
	}

	int i = 0;
	int count = 0;
	struct config_setting_t *it = NULL;

	while ((it = libconfig->setting_get_elem(mg_conf.root, i++)) != NULL) {
		if (mob->read_group_db_libconfig_sub(it, filepath))
			++count;
	}

	libconfig->destroy(&mg_conf);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", count, filepath);
	return true;
}

static bool mob_read_group_db_libconfig_sub(struct config_setting_t *it, const char *source)
{
	nullpo_retr(false, it);
	nullpo_retr(false, source);

	const char *name = config_setting_name(it);
	int group_id;

	if (!script->get_constant(name, &group_id)) {
		ShowWarning("%s: unknown monster group '%s' in \"%s\", skipping..\n", __func__, name, source);
		return false;
	}

	if (!config_setting_is_list(it)) {
		ShowWarning("%s: monster group '%s' in \"%s\" must be a list, skipping..\n", __func__, name, source);
		return false;
	}

	if (!mob->read_group_db_libconfig_sub_group(it, group_id, source))
		return false;

	return true;
}

static bool mob_read_group_db_libconfig_sub_group(struct config_setting_t *it, enum mob_groups group_id, const char *source)
{
	nullpo_retr(false, it);
	nullpo_retr(false, source);
	Assert_retr(false, group_id >= 0 && group_id < MOBG_MAX_GROUP);

	if (VECTOR_LENGTH(mob->mob_groups[group_id]) > 0) {
		VECTOR_CLEAR(mob->mob_groups[group_id]);
	}
	VECTOR_INIT(mob->mob_groups[group_id]);

	int i = 0;
	struct config_setting_t *entry = NULL;

	while ((entry = libconfig->setting_get_elem(it, i++)) != NULL) {
		int class_ = 0;
		const char *name = libconfig->setting_get_string_elem(entry, 0);

		if (!script->get_constant(name, &class_)) {
			ShowWarning("%s: Invalid monster '%s' in \"%s\", skipping..\n", __func__, name, source);
			continue;
		}

		struct mob_db *m = mob->db(class_);
		if (m == mob->dummy) {
			ShowWarning("%s: Invalid monster '%s' in \"%s\", skipping..\n", __func__, name, source);
			continue;
		}

		int rate = 0;
		if (config_setting_is_list(entry))
			rate = libconfig->setting_get_int_elem(entry, 1);
		else
			rate = 1;

		VECTOR_ENSURE(mob->mob_groups[group_id], 1, 1);
		VECTOR_PUSH(mob->mob_groups[group_id], class_);
		m->summonper[group_id] = rate;
	}

	return true;
}

/*==========================================
 * processes one mob_chat_db entry [SnakeDrak]
 * @param last_msg_id ensures that only one error message per mob id is printed
 *------------------------------------------*/
static bool mob_parse_row_chatdb(char **str, const char *source, int line, int *last_msg_id)
{
	char* msg;
	struct mob_chat *ms;
	int msg_id;
	size_t len;

	nullpo_retr(false, str);
	nullpo_retr(false, last_msg_id);
	msg_id = atoi(str[0]);

	if (msg_id <= 0 || msg_id > MAX_MOB_CHAT)
	{
		if (msg_id != *last_msg_id) {
			ShowError("mob_chat: Invalid chat ID: %d at %s, line %d\n", msg_id, source, line);
			*last_msg_id = msg_id;
		}
		return false;
	}

	if (mob->chat_db[msg_id] == NULL)
		mob->chat_db[msg_id] = (struct mob_chat*)aCalloc(1, sizeof (struct mob_chat));

	ms = mob->chat_db[msg_id];
	//MSG ID
	ms->msg_id=msg_id;
	//Color
	ms->color=(unsigned int)strtoul(str[1],NULL,0);
	//Message
	msg = str[2];
	len = strlen(msg);

	while( len && ( msg[len-1]=='\r' || msg[len-1]=='\n' ) )
	{// find EOL to strip
		len--;
	}

	if(len>(CHAT_SIZE_MAX-1))
	{
		if (msg_id != *last_msg_id) {
			ShowError("mob_chat: readdb: Message too long! Line %d, id: %d\n", line, msg_id);
			*last_msg_id = msg_id;
		}
		return false;
	}
	else if( !len )
	{
		ShowWarning("mob_parse_row_chatdb: Empty message for id %d.\n", msg_id);
		return false;
	}

	msg[len] = 0;  // strip previously found EOL
	safestrncpy(ms->msg, str[2], CHAT_SIZE_MAX);

	return true;
}

/*==========================================
 * mob_chat_db.txt reading [SnakeDrak]
 *-------------------------------------------------------------------------*/
static void mob_readchatdb(void)
{
	const char *arc = "mob_chat_db.txt";
	uint32 lines=0, count=0;
	char line[1024], filepath[280];
	int i, tmp=0;
	FILE *fp;
	snprintf(filepath, sizeof(filepath), "%s/%s", map->db_path, arc);
	fp=fopen(filepath, "r");
	if(fp == NULL) {
		ShowWarning("mob_readchatdb: File not found \"%s\", skipping.\n", filepath);
		return;
	}

	while(fgets(line, sizeof(line), fp)) {
		char *str[3], *p, *np;
		int j=0;

		lines++;
		if(line[0] == '/' && line[1] == '/')
			continue;
		memset(str, 0, sizeof(str));

		p=line;
		while(ISSPACE(*p))
			++p;
		if(*p == '\0')
			continue;// empty line
		for(i = 0; i <= 2; i++)
		{
			str[i] = p;
			if(i<2 && (np = strchr(p, ',')) != NULL) {
				*np = '\0'; p = np + 1; j++;
			}
		}

		if( j < 2 || str[2]==NULL)
		{
			ShowError("mob_readchatdb: Insufficient number of fields for skill at %s, line %u\n", arc, lines);
			continue;
		}

		if( !mob->parse_row_chatdb(str, filepath, lines, &tmp) )
			continue;

		count++;
	}
	fclose(fp);
	ShowStatus("Done reading '"CL_WHITE"%"PRIu32""CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", count, filepath);
}

/*==========================================
 * processes one mob_skill_db entry
 *------------------------------------------*/
static bool mob_skill_db_libconfig(const char *filename, bool ignore_missing)
{
	struct config_t mob_skill_conf;
	struct config_setting_t *it = NULL, *its = NULL, *mob_skill = NULL;
	char filepath[512];
	int i = 0;

	nullpo_retr(false, filename);

	snprintf(filepath, sizeof(filepath), "%s/%s", map->db_path, filename);

	if (!exists(filepath)) {
		if (!ignore_missing) {
			ShowError("mob_skill_db_libconfig: can't find file %s\n", filepath);
		}
		return 0;
	}

	if (!libconfig->load_file(&mob_skill_conf, filepath))
		return false;

	its = libconfig->lookup(&mob_skill_conf, "mob_skill_db");

	if (its != NULL && (mob_skill = libconfig->setting_get_elem(its, 0)) != NULL) {
		while ((it = libconfig->setting_get_elem(mob_skill, i++))) {
			mob->skill_db_libconfig_sub(it, i);
		}
	}

	libconfig->destroy(&mob_skill_conf);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", i, filepath);
	return true;
}

static bool mob_skill_db_libconfig_sub(struct config_setting_t *it, int n)
{
	int i = 0;
	int mob_id;
	struct config_setting_t *its;
	const char *name = config_setting_name(it);

	nullpo_retr(false, it);

	if (!*name) {
		ShowWarning("mob_skill_db_libconfig_sub: Invalid value for monster name, entry #%d, skipping.\n", n);
		return false;
	}
	if (!script->get_constant(name, &mob_id)) {
		ShowWarning("mob_skill_db_libconfig_sub: Invalid monster '%s', entry #%d, skipping.\n", name, n);
		return false;
	}
	if (mob_id > 0 && mob->db(mob_id) == mob->dummy) {
		ShowWarning("mob_skill_db_libconfig_sub: Non existant monster id %d, skipping.\n", mob_id);
		return false;
	}

	while ((its = libconfig->setting_get_elem(it, i++))) {
		mob->skill_db_libconfig_sub_skill(its, i, mob_id);
	}
	return true;
}

/**
 * Reads a single monster skill from DB.
 *
 * @param it The libconfig settings block, which contains the skill data.
 * @param n The skill data block's index within the parent monster block.
 * @param mob_id The monster's ID.
 * @return true on success, false on failure.
 *
 **/
static bool mob_skill_db_libconfig_sub_skill(struct config_setting_t *it, int n, int mob_id)
{
	nullpo_retr(false, it);
	Assert_retr(false, mob_id <= 0 || mob->db(mob_id) != mob->dummy);

	int skill_id = 0;
	const char *name = config_setting_name(it);
	const char *mob_str = (mob_id < 0) ? "global ID" : "monster";
	const char *mob_sprite = (mob_id > 0) ? mob->db(mob_id)->sprite : "";

	if ((skill_id = skill->name2id(name)) == 0) {
		ShowWarning("%s: Non existant skill %s in %s %s (%d), skipping.\n", __func__, name, mob_str, mob_sprite, mob_id);
		return false;
	}

	const char *skill_name = skill->get_name(skill_id);
	bool clearskills = false;

	// If ClearSkills flag is enabled clear all the previous skills.
	if (libconfig->setting_lookup_bool_real(it, "ClearSkills", &clearskills) == CONFIG_TRUE && clearskills) {
		if (mob_id < 0) {
			ShowError("%s: Global skill clearing is not supported, skipping. (Global ID %d, skill %d (%s).)\n",
				  __func__, mob_id, skill_id, skill_name);
			return false;
		}

		memset(mob->db_data[mob_id]->skill, 0, sizeof(struct mob_skill) * MAX_MOBSKILL);
		mob->db_data[mob_id]->maxskill = 0;
		return true;
	}

	struct mob_skill *ms;

	if (mob_id < 0) { // Prepare global skill. [Skotlex]
		struct mob_skill gms;
		memset(&gms, 0, sizeof (struct mob_skill));
		ms = &gms;
	} else {
		int idx = 0;

		ARR_FIND(0, MAX_MOBSKILL, idx, (ms = &mob->db_data[mob_id]->skill[idx])->skill_id == 0);

		if (idx == MAX_MOBSKILL) {
			ShowError("%s: Too many skills for monster %s (%d), skipping.\n", __func__, mob_sprite, mob_id);
			return false;
		}

		mob->db_data[mob_id]->maxskill = idx + 1;
	}

	ms->skill_id = skill_id;

	int i32 = MSS_ANY;
	if (map->setting_lookup_const(it, "SkillState", &i32) && (i32 < MSS_ANY || i32 > MSS_ANYTARGET)) {
		ShowWarning("%s: Invalid skill state %d for skill %d (%s) in %s %s (%d), defaulting to MSS_ANY.\n",
			    __func__, i32, skill_id, skill_name, mob_str, mob_sprite, mob_id);
		i32 = MSS_ANY;
	}
	ms->state = i32;

	int res = libconfig->setting_lookup_int(it, "SkillLevel", &i32);
	ms->skill_lv = (res == CONFIG_FALSE) ? 1 : cap_value(i32, 1, battle_config.mob_max_skilllvl);

	res = libconfig->setting_lookup_int(it, "Rate", &i32);
	ms->permillage = (res == CONFIG_FALSE) ? 1 : cap_value(i32, 1, 10000);

	// Apply battle_config modifier to rate (permillage).
	if (battle_config.mob_skill_rate != 100)
		ms->permillage = ms->permillage * battle_config.mob_skill_rate / 100;

	if (ms->permillage > 10000)
		ms->permillage = 10000;
	else if (ms->permillage == 0 && battle_config.mob_skill_rate != 0)
		ms->permillage = 1;

	res = libconfig->setting_lookup_int(it, "CastTime", &i32);
	ms->casttime = (res == CONFIG_FALSE) ? 0 : cap_value(i32, 0, MOB_MAX_CASTTIME);

	res = libconfig->setting_lookup_int(it, "Delay", &i32);
	ms->delay = (res == CONFIG_FALSE) ? 0 : cap_value(i32, 0, MOB_MAX_DELAY);

	// Apply battle_config modifier to delay.
	if (battle_config.mob_skill_delay != 100)
		ms->delay = ms->delay * battle_config.mob_skill_delay / 100;

	ms->delay = min(ms->delay, MOB_MAX_DELAY);

	res = libconfig->setting_lookup_bool(it, "Cancelable", &i32);
	ms->cancel = (res == CONFIG_FALSE) ? 0 : cap_value(i32, 0, 1);

	i32 = MST_TARGET;
	if (map->setting_lookup_const(it, "SkillTarget", &i32) && (i32 < MST_TARGET || i32 > MST_AROUND)) {
		ShowWarning("%s: Invalid skill target %d for skill %d (%s) in %s %s (%d), defaulting to MST_TARGET.\n",
			    __func__, i32, skill_id, skill_name, mob_str, mob_sprite, mob_id);
		i32 = MST_TARGET;
	}
	ms->target = i32;

	// Check the target condition for non-ground skills. (Ground skills can use every target.)
	if (skill->get_casttype2(skill->get_index(skill_id)) != CAST_GROUND && ms->target > MST_MASTER) {
		ShowWarning("%s: Wrong skill target %d for non-ground skill %d (%s) in %s %s (%d), defaulting to MST_TARGET.\n",
			    __func__, ms->target, skill_id, skill_name, mob_str, mob_sprite, mob_id);
		ms->target = MST_TARGET;
	}

	i32 = MSC_ALWAYS;
	if (map->setting_lookup_const(it, "CastCondition", &i32) && (i32 < MSC_ALWAYS || i32 > MSC_MAGICATTACKED)) {
		ShowWarning("%s: Invalid skill condition %d for skill id %d (%s) in %s %s (%d), defaulting to MSC_ALWAYS.\n",
			    __func__, i32, skill_id, skill_name, mob_str, mob_sprite, mob_id);
		i32 = MSC_ALWAYS;
	}
	ms->cond1 = i32;

	ms->cond2 = !map->setting_lookup_const(it, "ConditionData", &i32) ? 0 : cap_value(i32, SHRT_MIN, SHRT_MAX);

	for (int i = 0; i < 5; i++) {
		char valname[16];
		sprintf(valname, "val%1d", i);

		if (map->setting_lookup_const_mask(it, valname, &i32))
			ms->val[i] = i32;
	}

	if (ms->skill_id == NPC_EMOTION) {
		ms->val[1] &= MD_MASK;
		ms->val[2] &= MD_MASK;
		ms->val[3] &= MD_MASK;

		if (mob_id > 0 && (uint32)ms->val[1] == mob->db(mob_id)->status.mode) {
			ms->val[1] = MD_NONE;
			ms->val[4] = 1; // Request to return mode to normal.
		}
	}

	if (ms->skill_id == NPC_EMOTION_ON && mob_id > 0 && ms->val[1] != MD_NONE) {
		// Add a mode to the mob and remove aggressive mode if the new mode is passive.
		if ((ms->val[1] & MD_AGGRESSIVE) == 0)
			ms->val[3] |= MD_AGGRESSIVE;

		ms->val[2] |= (uint32)ms->val[1]; // Add the new mode.
		ms->val[1] = MD_NONE; // Do not "set" it.
	}

	res = map->setting_lookup_const(it, "Emotion", &i32);
	ms->emotion = res ? cap_value(i32, -1, SHRT_MAX) : -1;

	if (libconfig->setting_lookup_int(it, "ChatMsgID", &i32) == CONFIG_TRUE) {
		if (i32 <= 0 || i32 > MAX_MOB_CHAT || mob->chat_db[i32] == NULL) {
			ShowWarning("%s: Invalid message ID %d for skill %d (%s) in %s %s (%d), ignoring.\n",
				    __func__, i32, skill_id, skill_name, mob_str, mob_sprite, mob_id);
		} else {
			ms->msg_id = i32;
		}
	}

	if (mob_id < 0) { // Global skill assignment.
		mob_id = -mob_id;

		for (int i = 1; i < MAX_MOB_DB; i++) {
			if (mob->db_data[i] == NULL)
				continue;

			if ((mob->db_data[i]->status.mode & MD_BOSS) != 0) {
				if ((mob_id & 2) == 0) // Skill not for boss monsters.
					continue;
			} else {
				if ((mob_id & 1) == 0) // Skill not for normal monsters.
					continue;
			}

			int idx;

			ARR_FIND(0, MAX_MOBSKILL, idx, mob->db_data[i]->skill[idx].skill_id == 0);

			if (idx == MAX_MOBSKILL) {
				ShowError("%s: Too many skills for monster %d in global ID %d, skipping.\n",
					  __func__, i, -mob_id);
				continue;
			}

			memcpy(&mob->db_data[i]->skill[idx], ms, sizeof(struct mob_skill));
			mob->db_data[i]->maxskill = idx + 1;
		}
	}

	return true;
}

/*==========================================
 * mob_skill_db.txt reading
 *------------------------------------------*/
static void mob_readskilldb(void)
{

	const char *filename[] = {
		DBPATH"mob_skill_db.conf",
		"mob_skill_db2.conf"
	};
	int i;

	if (battle_config.mob_skill_rate == 0) {
		ShowStatus("Mob skill use disabled. Not reading mob skills.\n");
		return;
	}

	for (i = 0; i < ARRAYLENGTH(filename); ++i) {
		mob->skill_db_libconfig(filename[i], i > 0 ? true : false);
	}
}

/*==========================================
 * mob_race2_db.txt reading
 *------------------------------------------*/
static bool mob_readdb_race2(char *fields[], int columns, int current)
{
	int race, i;

	nullpo_retr(false, fields);
	race = atoi(fields[0]);

	if (race < RC2_NONE || race >= RC2_MAX) {
		ShowWarning("mob_readdb_race2: Unknown race2 %d.\n", race);
		return false;
	}

	for (i = 1; i < columns; i++) {
		int mobid = atoi(fields[i]);
		if (mob->db(mobid) == mob->dummy) {
			ShowWarning("mob_readdb_race2: Unknown mob id %d for race2 %d.\n", mobid, race);
			continue;
		}
		mob->db_data[mobid]->race2 = race;
	}
	return true;
}

/**
 * Read mob_item_ratio.txt
 */
static bool mob_readdb_itemratio(char *str[], int columns, int current)
{
	int nameid, ratio, i;
	struct item_drop_ratio *dropRatio;

	nullpo_retr(false, str);
	nameid = atoi(str[0]);

	if( itemdb->exists(nameid) == NULL )
	{
		ShowWarning("itemdb_read_itemratio: Invalid item id %d.\n", nameid);
		return false;
	}

	ratio = atoi(str[1]);

	dropRatio = mob->get_item_drop_ratio(nameid);
	if (dropRatio == NULL) {
		dropRatio = (struct item_drop_ratio*)aCalloc(1, sizeof(struct item_drop_ratio));
		mob->set_item_drop_ratio(nameid, dropRatio);
	}

	dropRatio->drop_ratio = ratio;
	for (i = 0; i < columns - 2; i++)
		dropRatio->mob_id[i] = atoi(str[i + 2]);

	return true;
}

/**
 * read all mob-related databases
 */
static void mob_load(bool minimal)
{
	if (minimal) {
		// Only read the mob db and option drops in minimal mode
		mob->read_optdrops_db();
		mob->readdb();
		return;
	}
	sv->readdb(map->db_path, "mob_item_ratio.txt", ',', 2, 2+MAX_ITEMRATIO_MOBS, -1, mob->readdb_itemratio); // must be read before mobdb
	mob->read_optdrops_db();
	mob->readchatdb();
	mob->readdb();
	mob->readskilldb();
	mob->mobavail_removal_notice();
	mob->read_group_db();
	sv->readdb(map->db_path, DBPATH"mob_race2_db.txt", ',', 2, 20, -1, mob->readdb_race2);
}

/**
 * @see DBApply
 */
static int mob_final_ratio_sub(union DBKey key, struct DBData *data, va_list ap)
{
	struct item_drop_ratio *ratio = DB->data2ptr(data);

	if (ratio)
		aFree(ratio);

	return 0;
}

static int mob_reload_sub_mob(struct mob_data *md, va_list args)
{
	nullpo_ret(md);
	md->db = mob_db(md->class_);

	status_calc_mob(md, SCO_FIRST);

	// If the view data was not overwritten manually
	if (md->vd != NULL && !md->vd_changed) {
		// Get the new view data from the mob database
		md->vd = mob_get_viewdata(md->class_);

		// If they are spawned right now
		if (md->bl.prev != NULL) {
			// Respawn all mobs on client side so that they are displayed correctly(if their view id changed)
			clif->clearunit_area(&md->bl, CLR_OUTSIGHT);
			clif->spawn(&md->bl);
		}
	}

	return 0;
}

static void mob_reload(void)
{
	int i;

	//Mob skills need to be cleared before re-reading them. [Skotlex]
	for (i = 0; i < MAX_MOB_DB; i++)
		if (mob->db_data[i] && !mob->is_clone(i)) {
			memset(&mob->db_data[i]->skill,0,sizeof(mob->db_data[i]->skill));
			mob->db_data[i]->maxskill=0;
		}

	// Clear item_drop_ratio_db
	for (i = 0; i < MAX_ITEMDB; i++) {
		if (item_drop_ratio_db[i]) {
			aFree(item_drop_ratio_db[i]);
			item_drop_ratio_db[i] = NULL;
		}
	}
	for (i = 0; i < MOBG_MAX_GROUP; i++) {
		VECTOR_CLEAR(mob->mob_groups[i]);
	}
	mob->item_drop_ratio_other_db->clear(mob->item_drop_ratio_other_db, mob->final_ratio_sub);

	mob->destroy_drop_groups();

	mob->load(false);
	map->foreachmob(mob->reload_sub_mob);
}

/**
 * Clears spawn related information for a script reload.
 */
static void mob_clear_spawninfo(void)
{
	int i;
	for (i = 0; i < MAX_MOB_DB; i++)
		if (mob->db_data[i])
			memset(&mob->db_data[i]->spawn,0,sizeof(mob->db_data[i]->spawn));
}

/*==========================================
 * Circumference initialization of mob
 *------------------------------------------*/
static int do_init_mob(bool minimal)
{
	// Initialize the mob database
	memset(mob->db_data,0,sizeof(mob->db_data)); //Clear the array
	mob->db_data[0] = (struct mob_db*)aCalloc(1, sizeof (struct mob_db)); //This mob is used for random spawns
	mob->makedummymobdb(0); //The first time this is invoked, it creates the dummy mob
	item_drop_ers = ers_new(sizeof(struct item_drop),"mob.c::item_drop_ers",ERS_OPT_CLEAN);
	item_drop_list_ers = ers_new(sizeof(struct item_drop_list),"mob.c::item_drop_list_ers",ERS_OPT_NONE);

	mob->load(minimal);

	if (minimal)
		return 0;

	timer->add_func_list(mob->delayspawn,"mob_delayspawn");
	timer->add_func_list(mob->delay_item_drop,"mob_delay_item_drop");
	timer->add_func_list(mob->ai_hard,"mob_ai_hard");
	timer->add_func_list(mob->ai_lazy,"mob_ai_lazy");
	timer->add_func_list(mob->timer_delete,"mob_timer_delete");
	timer->add_func_list(mob->spawn_guardian_sub,"mob_spawn_guardian_sub");
	timer->add_func_list(mob->respawn,"mob_respawn");
	timer->add_func_list(mob->mvptomb_delayspawn, "mvptomb_delayspawn");
	timer->add_interval(timer->gettick()+MIN_MOBTHINKTIME,mob->ai_hard,0,0,MIN_MOBTHINKTIME);
	timer->add_interval(timer->gettick()+MIN_MOBTHINKTIME*10,mob->ai_lazy,0,0,MIN_MOBTHINKTIME*10);

	return 0;
}

static void mob_destroy_mob_db(int index)
{
	struct mob_db *data;
	Assert_retv(index >= 0 && index <= MAX_MOB_DB);
	data = mob->db_data[index];
	HPM->data_store_destroy(&data->hdata);
	aFree(data);
	mob->db_data[index] = NULL;
}

/**
 * Unloads option drop group database
 */
static void mob_destroy_drop_groups(void)
{
	for (int i = 0; i < mob->opt_drop_groups_count; i++) {
		struct optdrop_group *group = &mob->opt_drop_groups[i];

		for (int j = 0; j < group->optslot_count; j++) {
			aFree(group->optslot[j].options);
		}
	}

	aFree(mob->opt_drop_groups);
}

/*==========================================
 * Clean memory usage.
 *------------------------------------------*/
static int do_final_mob(void)
{
	int i;
	if (mob->dummy)
	{
		aFree(mob->dummy);
		mob->dummy = NULL;
	}
	for (i = 0; i <= MAX_MOB_DB; i++)
	{
		if (mob->db_data[i] != NULL)
		{
			mob->destroy_mob_db(i);
		}
	}
	mob->destroy_drop_groups();
	for (i = 0; i <= MAX_MOB_CHAT; i++)
	{
		if (mob->chat_db[i] != NULL)
		{
			aFree(mob->chat_db[i]);
			mob->chat_db[i] = NULL;
		}
	}
	for (i = 0; i < MAX_ITEMDB; i++)
	{
		if (item_drop_ratio_db[i] != NULL)
		{
			aFree(item_drop_ratio_db[i]);
			item_drop_ratio_db[i] = NULL;
		}
	}
	for (i = 0; i < MOBG_MAX_GROUP; i++) {
		VECTOR_CLEAR(mob->mob_groups[i]);
	}
	mob->item_drop_ratio_other_db->clear(mob->item_drop_ratio_other_db, mob->final_ratio_sub);
	db_destroy(mob->item_drop_ratio_other_db);
	ers_destroy(item_drop_ers);
	ers_destroy(item_drop_list_ers);
	return 0;
}

void mob_defaults(void)
{
	// Defines the Manuk/Splendide/Mora mob groups for the status reductions [Epoque & Frost]
	const int mob_manuk[8] = {
		MOBID_TATACHO,
		MOBID_CENTIPEDE,
		MOBID_NEPENTHES,
		MOBID_HILLSRION,
		MOBID_HARDROCK_MOMMOTH,
		MOBID_G_TATACHO,
		MOBID_G_HILLSRION,
		MOBID_CENTIPEDE_LARVA,
	};
	const int mob_splendide[5] = {
		MOBID_TENDRILRION,
		MOBID_CORNUS,
		MOBID_NAGA,
		MOBID_LUCIOLA_VESPA,
		MOBID_PINGUICULA,
	};
	const int mob_mora[5] = {
		MOBID_POM_SPIDER,
		MOBID_ANGRA_MANTIS,
		MOBID_PARUS,
		MOBID_LITTLE_FATUM,
		MOBID_MIMING,
	};

	mob = &mob_s;

	memset(mob->db_data, 0, sizeof(mob->db_data));
	mob->dummy = NULL;
	memset(mob->chat_db, 0, sizeof(mob->chat_db));

	memcpy(mob->manuk, mob_manuk, sizeof(mob->manuk));
	memcpy(mob->splendide, mob_splendide, sizeof(mob->splendide));
	memcpy(mob->mora, mob_mora, sizeof(mob->mora));

	item_drop_ratio_other_db = idb_alloc(DB_OPT_BASE);
	mob->item_drop_ratio_db = item_drop_ratio_db;
	mob->item_drop_ratio_other_db = item_drop_ratio_other_db;

	/* */
	mob->reload = mob_reload;
	mob->reload_sub_mob = mob_reload_sub_mob;
	mob->init = do_init_mob;
	mob->final = do_final_mob;
	/* */
	mob->db = mob_db;
	mob->chat = mob_chat;
	mob->makedummymobdb = mob_makedummymobdb;
	mob->spawn_guardian_sub = mob_spawn_guardian_sub;
	mob->skill_id2skill_idx = mob_skill_id2skill_idx;
	mob->db_searchname = mobdb_searchname;
	mob->db_searchname_array_sub = mobdb_searchname_array_sub;
	mob->mvptomb_create = mvptomb_create;
	mob->mvptomb_destroy = mvptomb_destroy;
	mob->mvptomb_spawn_delayed = mvptomb_spawn_delayed;
	mob->mvptomb_delayspawn = mvptomb_delayspawn;
	mob->db_searchname_array = mobdb_searchname_array;
	mob->db_checkid = mobdb_checkid;
	mob->get_viewdata = mob_get_viewdata;
	mob->set_dynamic_viewdata = mob_set_dynamic_viewdata;
	mob->free_dynamic_viewdata = mob_free_dynamic_viewdata;
	mob->parse_dataset = mob_parse_dataset;
	mob->spawn_dataset = mob_spawn_dataset;
	mob->get_random_id = mob_get_random_id;
	mob->ksprotected = mob_ksprotected;
	mob->once_spawn_sub = mob_once_spawn_sub;
	mob->once_spawn = mob_once_spawn;
	mob->once_spawn_area = mob_once_spawn_area;
	mob->spawn_guardian = mob_spawn_guardian;
	mob->spawn_bg = mob_spawn_bg;
	mob->can_reach = mob_can_reach;
	mob->linksearch = mob_linksearch;
	mob->delayspawn = mob_delayspawn;
	mob->setdelayspawn = mob_setdelayspawn;
	mob->count_sub = mob_count_sub;
	mob->spawn = mob_spawn;
	mob->can_changetarget = mob_can_changetarget;
	mob->target = mob_target;
	mob->ai_sub_hard_activesearch = mob_ai_sub_hard_activesearch;
	mob->ai_sub_hard_changechase = mob_ai_sub_hard_changechase;
	mob->ai_sub_hard_bg_ally = mob_ai_sub_hard_bg_ally;
	mob->ai_sub_hard_lootsearch = mob_ai_sub_hard_lootsearch;
	mob->warpchase_sub = mob_warpchase_sub;
	mob->is_in_battle_state = mob_is_in_battle_state;
	mob->ai_sub_hard_slavemob = mob_ai_sub_hard_slavemob;
	mob->unlocktarget = mob_unlocktarget;
	mob->randomwalk = mob_randomwalk;
	mob->warpchase = mob_warpchase;
	mob->ai_sub_hard = mob_ai_sub_hard;
	mob->ai_sub_hard_timer = mob_ai_sub_hard_timer;
	mob->ai_sub_foreachclient = mob_ai_sub_foreachclient;
	mob->ai_sub_lazy = mob_ai_sub_lazy;
	mob->ai_lazy = mob_ai_lazy;
	mob->ai_hard = mob_ai_hard;
	mob->setdropitem_options = mob_setdropitem_options;
	mob->setdropitem = mob_setdropitem;
	mob->setlootitem = mob_setlootitem;
	mob->delay_item_drop = mob_delay_item_drop;
	mob->item_drop = mob_item_drop;
	mob->timer_delete = mob_timer_delete;
	mob->deleteslave_sub = mob_deleteslave_sub;
	mob->deleteslave = mob_deleteslave;
	mob->respawn = mob_respawn;
	mob->log_damage = mob_log_damage;
	mob->damage = mob_damage;
	mob->dead = mob_dead;
	mob->revive = mob_revive;
	mob->guardian_guildchange = mob_guardian_guildchange;
	mob->random_class = mob_random_class;
	mob->class_change = mob_class_change;
	mob->heal = mob_heal;
	mob->warpslave_sub = mob_warpslave_sub;
	mob->warpslave = mob_warpslave;
	mob->countslave_sub = mob_countslave_sub;
	mob->countslave = mob_countslave;
	mob->summonslave = mob_summonslave;
	mob->getfriendhprate_sub = mob_getfriendhprate_sub;
	mob->getfriendhprate = mob_getfriendhprate;
	mob->getmasterhpltmaxrate = mob_getmasterhpltmaxrate;
	mob->getfriendstatus_sub = mob_getfriendstatus_sub;
	mob->getfriendstatus = mob_getfriendstatus;
	mob->use_skill = mob_use_skill;
	mob->use_skill_event = mob_use_skill_event;
	mob->is_clone = mob_is_clone;
	mob->clone_spawn = mob_clone_spawn;
	mob->clone_delete = mob_clone_delete;
	mob->drop_adjust = mob_drop_adjust;
	mob->item_dropratio_adjust = item_dropratio_adjust;
	mob->read_optdrops_option = mob_read_optdrops_option;
	mob->read_optdrops_optslot = mob_read_optdrops_optslot;
	mob->read_optdrops_group = mob_read_optdrops_group;
	mob->read_optdrops_db = mob_read_optdrops_db;
	mob->get_const = mob_get_const;
	mob->db_validate_entry = mob_db_validate_entry;
	mob->readdb = mob_readdb;
	mob->read_libconfig = mob_read_libconfig;
	mob->read_db_additional_fields = mob_read_db_additional_fields;
	mob->read_db_sub = mob_read_db_sub;
	mob->read_db_drops_sub = mob_read_db_drops_sub;
	mob->read_db_mvpdrops_sub = mob_read_db_mvpdrops_sub;
	mob->read_db_mode_sub = mob_read_db_mode_sub;
	mob->read_db_drops_option = mob_read_db_drops_option;
	mob->read_db_stats_sub = mob_read_db_stats_sub;
	mob->read_db_viewdata_sub = mob_read_db_viewdata_sub;
	mob->name_constants = mob_name_constants;
	mob->mobavail_removal_notice = mob_mobavail_removal_notice;
	mob->parse_row_chatdb = mob_parse_row_chatdb;
	mob->readchatdb = mob_readchatdb;
	mob->readskilldb = mob_readskilldb;
	mob->readdb_race2 = mob_readdb_race2;
	mob->readdb_itemratio = mob_readdb_itemratio;
	mob->load = mob_load;
	mob->get_item_drop_ratio = mob_get_item_drop_ratio;
	mob->set_item_drop_ratio = mob_set_item_drop_ratio;
	mob->final_ratio_sub = mob_final_ratio_sub;
	mob->clear_spawninfo = mob_clear_spawninfo;
	mob->destroy_mob_db = mob_destroy_mob_db;
	mob->destroy_drop_groups = mob_destroy_drop_groups;
	mob->skill_db_libconfig = mob_skill_db_libconfig;
	mob->skill_db_libconfig_sub = mob_skill_db_libconfig_sub;
	mob->skill_db_libconfig_sub_skill = mob_skill_db_libconfig_sub_skill;
	mob->read_group_db = mob_read_group_db;
	mob->read_group_db_libconfig = mob_read_group_db_libconfig;
	mob->read_group_db_libconfig_sub = mob_read_group_db_libconfig_sub;
	mob->read_group_db_libconfig_sub_group = mob_read_group_db_libconfig_sub_group;
}
