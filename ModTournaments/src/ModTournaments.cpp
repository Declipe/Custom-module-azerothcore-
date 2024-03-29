#include "Config.h"
#include "Player.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "ScriptedGossip.h"
#include "WorldPacket.h"
#include "ObjectMgr.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "World.h"
#include "MapMgr.h"
#include "WorldSession.h"
#include "Group.h"
#include "AchievementMgr.h"
#include "ObjectAccessor.h"
#include "Unit.h"
#include "SharedDefines.h"
#include "Creature.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Language.h"
#include "Chat.h"
#include "Channel.h"
#include "CreatureTextMgr.h"
#include "SmartScriptMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include <fstream>
#include <unordered_map>
#include "ChatCommand.h"

#define SQL_TEMPLATE "SELECT `entry`, `level`, `time_limit`, `say_start`, `say_win`, `say_lose`, `chest_id`, `point_id`, `req_quest_id`, `kill_credit`, `menu_string` FROM `world_tournaments` ORDER BY `entry`, `level` DESC"
#define SQL_CREATURE "SELECT `id`, `tournament_entry`, `tournament_level`, `entry`, `count`, `from_point_id`, `to_point_id`, `time` FROM `world_tournament_creature` ORDER BY `tournament_level`, `time`"
#define SQL_POINTS "SELECT `id`, `map_id`, `x`, `y`, `z`, `o` FROM `world_tournament_points`"

struct PointOnTournament
{
    uint32 id;
    uint32 map;
    float x;
    float y;
    float z;
    float o;
};

struct TournamentCreature
{
    uint32 id;
    uint32 tournament;
    uint32 level;
    uint32 entry;
    uint32 count;
    uint32 point;
    uint32 move;
    uint32 time;
    bool spawn;
};

typedef std::list<TournamentCreature*> TournamentCreatureList;

struct TournamentLevel
{
    uint32 entry;
    uint32 level;
    uint32 timeLimit;
    uint32 time;
    uint32 sayStart; //string_id
    uint32 sayWin; //string_id
    uint32 sayLose; //string_id
    uint32 chest;
    uint32 point;
    uint32 reqQuest;
    uint32 killCredit;
    uint32 menuString;
    TournamentCreatureList creatures;
};

typedef std::map<uint32, TournamentLevel*> TournamentLevels;
typedef std::list<Creature*> CreatureList;

struct TournamentTemplate
{
    uint32 entry;
    TournamentLevels levels;
    uint32 level;
    bool inProgress;
    TournamentLevel* current;
    CreatureList creatures;
    Creature* organizer;
    Player* starter;
};

typedef std::map<uint32, PointOnTournament const*> TournamentPoints;
typedef std::map<uint32, TournamentTemplate*> Tournaments;


static bool TournamentEnable = false;
static bool TournamentDebug = false;
static bool TournamentLadder = false;
static uint32  TournamentLadderText = 0;

class TournamentManager
{
public:
    TournamentManager() { loaded = false; }
    ~TournamentManager() { clear(); }

    void load(bool reload = false);

    void start(uint32 entry, uint32 level, Player* player = nullptr);
    void stop(uint32 entry, bool win = false);
    bool checkEnd(uint32 entry);
    void addGossip(Creature* creature, Player* player);

    void update(uint32 diff);
    void updateTournament(uint32 entry, uint32 diff);

    void clear();
    void reset(uint32 entry);

    bool pointExists(uint32 id);

    bool existsLevelTournament(uint32 entry, uint32 level);
    bool existsTournament(uint32 entry);

    bool existsAlive(uint32 entry);

    PointOnTournament const* getPoint(uint32 id);
    TournamentTemplate* getTournament(uint32 entry);
    TournamentLevel* getTournamentLevel(uint32 entry, uint32 level);
    void createTournament(uint32 entry);

    bool isProgress(uint32 entry);
    uint32 getLevel(uint32 entry);

    void setTournamentOrganizer(uint32 entry, Creature* npc);

private:
    Tournaments tournaments;
    TournamentPoints points;
    bool loaded;
};

TournamentTemplate* TournamentManager::getTournament(uint32 entry)
{
    Tournaments::const_iterator itr = tournaments.find(entry);
    if (itr != tournaments.end())
        return (*itr).second;
    return nullptr;
}

TournamentLevel* TournamentManager::getTournamentLevel(uint32 entry, uint32 level)
{
    if (TournamentDebug)
        LOG_ERROR("server", "TournamentManager::getTournamentLevel entry: %u level: %u", entry, level);

    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
    {
        if (TournamentDebug)
            LOG_ERROR("server", "TournamentManager::getTournamentLevel not exists entry: %u level: %u", entry, level);
        return nullptr;
    }

    TournamentLevels::const_iterator itr = tournament->levels.find(level);
    if (itr != tournament->levels.end())
        return (*itr).second;

    if (TournamentDebug)
        LOG_ERROR("server", "TournamentManager::getTournamentLevel not exists level: %u for entry: %u", level, entry);
    return nullptr;
}

PointOnTournament const* TournamentManager::getPoint(uint32 id)
{
    if (!pointExists(id))
        return nullptr;

    return points[id];
}

bool TournamentManager::pointExists(uint32 id)
{
    TournamentPoints::const_iterator itr = points.find(id);
    if (itr != points.end())
        return true;
    return false;
}

void TournamentManager::setTournamentOrganizer(uint32 entry, Creature* npc)
{
    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return;

    tournament->organizer = npc;
}

bool TournamentManager::isProgress(uint32 entry)
{
    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return false;

    return tournament->inProgress;
}

uint32 TournamentManager::getLevel(uint32 entry)
{
    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return 0;

    return tournament->level;
}

bool TournamentManager::existsTournament(uint32 entry)
{
    return getTournament(entry) != nullptr;
}

bool TournamentManager::existsLevelTournament(uint32 entry, uint32 level)
{
    return getTournamentLevel(entry, level) != nullptr;
}

void TournamentManager::createTournament(uint32 entry)
{
    if (existsTournament(entry))
        return;

    TournamentTemplate* tournamentTemplate = new TournamentTemplate();
    tournamentTemplate->inProgress = false;
    tournamentTemplate->level = 0;
    tournamentTemplate->entry = entry;
    tournaments[entry] = tournamentTemplate;
}

void TournamentManager::load(bool reload)
{
    if (loaded && !reload)
        return;

    loaded = true;

    clear();

    LOG_INFO("server.loading", "Loading Tournaments...");
    uint32 oldMSTime;
    QueryResult result;
    uint16 count;

    /*============ POINTS ============*/
    if (TournamentDebug)
        LOG_INFO("server.loading", "Start loadig points");
    oldMSTime = getMSTime();
    result = WorldDatabase.Query(SQL_POINTS);
    count = 0;
    if (!result)
        LOG_INFO("server.loading", ">> `tournament_points` is empty");
    do
    {
        Field* fields = result->Fetch();

        PointOnTournament* point = new PointOnTournament();
        point->id = fields[0].Get<uint32>();
        point->map = fields[1].Get<uint32>();
        point->x = fields[2].Get<float>();
        point->y = fields[3].Get<float>();
        point->z = fields[4].Get<float>();
        point->o = fields[5].Get<float>();

        if (!MapMgr::IsValidMapCoord(point->map, point->x, point->y, point->z, point->o))
        {
            LOG_INFO("server.loading", "Wrong position for point id %u in `world_tournament_points` table, ignoring.", point->id);
            delete point;
            continue;
        }

        points[point->id] = point;
        ++count;
    } while (result->NextRow());
    LOG_INFO("server.loading", ">> Loaded %u points for TournamentManager in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
    if (TournamentDebug)
        LOG_INFO("server.loading", "End loadig points");
    /*============ POINTS ============*/

   /*============ TEMPLATES ============*/
    if (TournamentDebug)
        LOG_INFO("server.loading", "Start loadig templates");
    oldMSTime = getMSTime();
    result = WorldDatabase.Query(SQL_TEMPLATE);
    count = 0;
    if (!result)
        LOG_INFO("server.loading", ">> `world_tournaments` is empty");
    do
    {
        Field* fields = result->Fetch();

        TournamentLevel* level = new TournamentLevel();
        level->entry = fields[0].Get<uint32>();
        level->level = fields[1].Get<uint32>();
        level->timeLimit = fields[2].Get<uint32>();
        level->sayStart = fields[3].Get<uint32>();
        level->sayWin = fields[4].Get<uint32>();
        level->sayLose = fields[5].Get<uint32>();
        level->chest = fields[6].Get<uint32>();
        level->point = fields[7].Get<uint32>();
        level->reqQuest = fields[8].Get<uint32>();
        level->killCredit = fields[9].Get<uint32>();
        level->menuString = fields[10].Get<uint32>();

        if (level->point && !pointExists(level->point))
        {
            LOG_ERROR("server", "Wrong point_id %u for level %u in `world_tournaments` table, ignoring.", level->point, level->level);
            level->point = 0;
        }

        if (level->chest && !sObjectMgr->GetGameObjectTemplate(level->chest))
        {
            LOG_ERROR("server", "Wrong chest %u for level %u in `world_tournaments` table, ignoring.", level->chest, level->level);
            level->chest = 0;
        }

        if (level->reqQuest && !sObjectMgr->GetQuestTemplate(level->reqQuest))
        {
            LOG_ERROR("server", "Wrong reqQuest %u for level %u in `world_tournaments` table, ignoring.", level->reqQuest, level->level);
            level->reqQuest = 0;
        }

        if (level->menuString && !sObjectMgr->GetAcoreString(level->menuString))
        {
            LOG_ERROR("server","Wrong menuString %u for level %u in `world_tournaments` table, ignoring.", level->menuString, level->level);
            level->menuString = 0;
        }

        if (!existsTournament(level->entry))
            createTournament(level->entry);

        if (existsLevelTournament(level->entry, level->level))
        {
            delete level;
            continue;
        }

        tournaments[level->entry]->levels[level->level] = level;
        ++count;
    } while (result->NextRow());
    LOG_INFO("server.loading", ">> Loaded %u templates for TournamentManager in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
    if (TournamentDebug)
        LOG_INFO("server.loading", "End loadig templates");
    /*============ TEMPLATES ============*/

   /*============ CREATURES ============*/
    if (TournamentDebug)
        LOG_INFO("server.loading", "Start loadig creatures");
    oldMSTime = getMSTime();
    result = WorldDatabase.Query(SQL_CREATURE);
    count = 0;
    if (!result)
        LOG_INFO("server.loading", ">> `world_tournament_creature` is empty");
    do
    {
        Field* fields = result->Fetch();

        TournamentCreature* creature = new TournamentCreature();
        creature->id = fields[0].Get<uint32>();
        creature->tournament = fields[1].Get<uint32>();
        creature->level = fields[2].Get<uint32>();
        creature->entry = fields[3].Get<uint32>();
        creature->count = fields[4].Get<uint32>();
        creature->point = fields[5].Get<uint32>();
        creature->move = fields[6].Get<uint32>();
        creature->time = fields[7].Get<uint32>();
        creature->spawn = false;

        if (TournamentDebug)
            LOG_INFO("server.loading", "TournamentManager::load -> check creature %u", creature->id);

        if (!existsLevelTournament(creature->tournament, creature->level))
        {
            LOG_ERROR("server","Wrong tournament %u level %u for creature id %u in `world_tournament_creature` table, ignoring.", creature->tournament, creature->level, creature->id);
            delete creature;
            continue;
        }

        if (!pointExists(creature->point))
        {
            LOG_ERROR("server","Wrong from point %u for creature id %u in `world_tournament_creature` table, ignoring.", creature->point, creature->id);
            delete creature;
            continue;
        }

        if (!pointExists(creature->move))
        {
            LOG_ERROR("server","Wrong to point %u for creature id %u in `world_tournament_creature` table, ignoring.", creature->move, creature->id);
            creature->move = 0;
        }

        if (!sObjectMgr->GetCreatureTemplate(creature->entry))
        {
            LOG_ERROR("server", "Wrong entry %u for creature id %u in `world_tournament_creature` table, ignoring.", creature->entry, creature->id);
            delete creature;
            continue;
        }

        if (TournamentDebug)
            LOG_INFO("server.loading", "TournamentManager::load -> end check creature %u", creature->id);

        tournaments[creature->tournament]->levels[creature->level]->creatures.push_back(creature);

        if (TournamentDebug)
            LOG_INFO("server.loading", "TournamentManager::load -> creature %u added", creature->id);

        ++count;
    } while (result->NextRow());
    LOG_INFO("server.loading", ">> Loaded %u creatures for TournamentManager in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
    if (TournamentDebug)
        LOG_INFO("server.loading", "end loadig creatures");
    /*============ CREATURES ============*/
}

void TournamentManager::clear()
{
    if (TournamentDebug)
        //TC_LOG_ERROR("misc", "TournamentManager::clear");

        for (TournamentPoints::const_iterator itr = points.begin(); itr != points.end(); ++itr)
            delete (*itr).second;
    points.clear();

    if (!tournaments.empty())
    {
        for (Tournaments::const_iterator itr = tournaments.begin(); itr != tournaments.end(); ++itr)
        {
            if (!(*itr).second->creatures.empty())
                for (CreatureList::const_iterator it = (*itr).second->creatures.begin(); it != (*itr).second->creatures.end(); ++it)
                    (*it)->DespawnOrUnsummon();
            (*itr).second->creatures.clear();

            for (TournamentLevels::const_iterator it = (*itr).second->levels.begin(); it != (*itr).second->levels.end(); ++it)
            {
                for (TournamentCreatureList::const_iterator i = it->second->creatures.begin(); i != it->second->creatures.end(); ++i)
                    delete* i;
                it->second->creatures.clear();
                delete (*it).second;
            }
            (*itr).second->levels.clear();
        }
    }
}

void TournamentManager::reset(uint32 entry)
{
    if (TournamentDebug)
        LOG_INFO("server.loading", "TournamentManager::reset: %u", entry);


    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return;

    if (tournament->level && !tournament->current->creatures.empty())
        for (TournamentCreatureList::const_iterator itr = tournament->current->creatures.begin(); itr != tournament->current->creatures.end(); ++itr)
            (*itr)->spawn = false;

    if (!tournament->creatures.empty())
        for (CreatureList::const_iterator itr = tournament->creatures.begin(); itr != tournament->creatures.end(); ++itr)
            (*itr)->DespawnOrUnsummon();
    //me->DespawnOrUnsummon();
    tournament->creatures.clear();
}

void TournamentManager::start(uint32 entry, uint32 level, Player* player)
{
    if (TournamentDebug)
        LOG_INFO("server.loading", "TournamentManager::start tournament: %u level: %u", entry, level);

    if (!existsLevelTournament(entry, level))
    {
        if (TournamentDebug)
            LOG_INFO("server.loading", "TournamentManager::start, not exists tournament/level: %u/%u", entry, level);
        return;
    }

    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
    {
        if (TournamentDebug)
            LOG_ERROR("server.loading", "TournamentManager::start, not exists tournament: %u", entry);
        return;
    }

    if (tournament->inProgress)
    {
        if (TournamentDebug)
            LOG_INFO("server.loading", "TournamentManager::start, tournament: %u in progress!!", entry);
        return;
    }

    tournament->starter = player;

    if (tournament->organizer)
    {
        tournament->organizer->RemoveFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
        tournament->organizer->SetVisible(false);
    }

    tournament->current = tournament->levels[level];
    tournament->level = level;

    reset(entry);

    tournament->current->time = 0;
    tournament->inProgress = true;

    if (tournament->organizer && tournament->current->sayStart)
        sCreatureTextMgr->SendChat(tournament->organizer, tournament->current->sayStart);
}

void TournamentManager::updateTournament(uint32 entry, uint32 diff)
{
    //if (TournamentDebug)
        //TC_LOG_ERROR("misc", "TournamentManager::updateTournament entry: %u", entry);

    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return;

    //if (TournamentDebug)
        //TC_LOG_ERROR("misc", "TournamentManager::updateTournament entry: %u Tournament Exists!!!", entry);

    if (!tournament->inProgress)
        return;

    //if (TournamentDebug)
        //TC_LOG_ERROR("misc", "TournamentManager::updateTournament entry: %u In Progress!!!", entry);

    tournament->current->time += diff;

    if (checkEnd(entry))
        return;

    for (TournamentCreatureList::const_iterator itr = tournament->current->creatures.begin(); itr != tournament->current->creatures.end(); ++itr)
        if (!(*itr)->spawn && tournament->current->time >= (*itr)->time)
        {
            if (TournamentDebug)
                LOG_INFO("server.loading", "TournamentManager::update -> spawn %u", (*itr)->entry);

            PointOnTournament const* point = getPoint((*itr)->point);
            PointOnTournament const* move = nullptr;
            if ((*itr)->move)
                move = getPoint((*itr)->move);

            if (tournament->organizer && point)
            {
                for (uint32 i = 0; i < (*itr)->count; ++i)
                {
                    //Creature* summon = tournament->organizer->SummonCreature((*itr)->entry, point->x, point->y, point->z, point->o, TEMPSUMMON_TIMED_DESPAWN, Milliseconds(tournament->current->timeLimit) + 10s);
                    Creature* summon = tournament->organizer->SummonCreature((*itr)->entry, point->x, point->y, point->z, point->o, TEMPSUMMON_TIMED_DESPAWN, tournament->current->timeLimit + 10000);
                    if (move)
                    {
                        summon->SetHomePosition(move->x, move->y, move->z, move->o);
                        //summon->GetMotionMaster()->MovePoint(0, move->x, move->y, move->z);
                        summon->GetMotionMaster()->MovePoint(0, move->x, move->y, move->z);
                    }
                    else if (tournament->starter && tournament->starter->IsAlive())
                    {
                        summon->SetHomePosition(point->x, point->y, point->z, point->o);
                        summon->AI()->AttackStart(tournament->starter);
                    }
                    tournament->creatures.push_back(summon);
                }
            }
            (*itr)->spawn = true;
        }
}

void TournamentManager::update(uint32 diff)
{
    if (tournaments.empty())
        return;


    for (Tournaments::const_iterator itr = tournaments.begin(); itr != tournaments.end(); ++itr)
        updateTournament((*itr).second->entry, diff);
}

void TournamentManager::stop(uint32 entry, bool win)
{
    if (TournamentDebug)
        LOG_INFO("server.loading", "TournamentManager::stop win: %i", int(win));

    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return;

    if (tournament->organizer)
    {
        tournament->organizer->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP); //TEST
        tournament->organizer->SetVisible(true);
    }

    tournament->inProgress = false;
    //reset(tournament->entry);
    reset(entry);

    if (!win)
    {
        if (tournament->organizer && tournament->current->sayLose)
            sCreatureTextMgr->SendChat(tournament->organizer, tournament->current->sayLose);

        if (TournamentLadder)
            tournament->level = 0;
        return;
    }

    if (tournament->organizer && tournament->current->sayWin)
        sCreatureTextMgr->SendChat(tournament->organizer, tournament->current->sayWin);

    if (tournament->current->chest && tournament->current->point)
    {
        PointOnTournament const* point = getPoint(tournament->current->point);
        if (tournament->organizer && point)
            tournament->organizer->SummonGameObject(tournament->current->chest, point->x, point->y, point->z, point->o, 0, 0, 0, 0, 300);
    }

    if (tournament->organizer && tournament->current->killCredit)
    {
        ObjectList* units = new ObjectList();
        Acore::AllWorldObjectsInRange u_check(tournament->organizer, 60.0f);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(tournament->organizer, *units, u_check);
        //tournament->organizer->VisitNearbyObject(60.0f, searcher);
        Cell::VisitAllObjects(tournament->organizer, searcher, 60.0f);

        if (!units->empty())
            for (ObjectList::const_iterator itr = units->begin(); itr != units->end(); ++itr)
                if ((*itr)->GetTypeId() == TYPEID_PLAYER)
                    ((Player*)(*itr))->KilledMonsterCredit(tournament->current->killCredit);

        delete units;
    }
}

bool TournamentManager::existsAlive(uint32 entry)
{
    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return false;

    for (CreatureList::const_iterator itr = tournament->creatures.begin(); itr != tournament->creatures.end(); ++itr)
        if ((*itr)->IsAlive())
            return true;

    return false;
}

bool TournamentManager::checkEnd(uint32 entry)
{
    //if (TournamentDebug)
        //TC_LOG_ERROR("TournamentManager::checkEnd entry: %u", entry);

    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return true;

    if (tournament->current->time >= tournament->current->timeLimit)
    {
        stop(entry);
        return true;
    }

    for (TournamentCreatureList::const_iterator itr = tournament->current->creatures.begin(); itr != tournament->current->creatures.end(); ++itr)
        if (!(*itr)->spawn)
            return false;

    if (existsAlive(entry))
        return false;

    stop(entry, true);

    return true;
}

void TournamentManager::addGossip(Creature* creature, Player* player)
{
    uint32 entry = creature->GetOriginalEntry();
    TournamentTemplate* tournament = getTournament(entry);
    if (!tournament)
        return;

    if (tournament->inProgress)
        return;

    if (TournamentDebug)
        LOG_INFO("server.loading", "TournamentManager::addGossip");

    char gossipTextFormat[100];

    if (TournamentLadder)
    {
        if (TournamentDebug)
            LOG_INFO("server.loading", "TournamentManager::addGossip -> TournamentLadder: level %u", tournament->level);

        uint32 newLevel = tournament->level + 1;

        if (!existsLevelTournament(entry, newLevel))
            newLevel = 1;

        if (TournamentLadderText)
            snprintf(gossipTextFormat, 100, sObjectMgr->GetAcoreStringForDBCLocale(TournamentLadderText), newLevel);
        else
            snprintf(gossipTextFormat, 100, "[%u] Next level", newLevel);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, gossipTextFormat, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + newLevel);
        return;
    }

    uint8 i = 0;
    uint8 limit = 9;

    for (TournamentLevels::const_iterator itr = tournament->levels.begin(); itr != tournament->levels.end(); ++itr)
    {
        if (i >= limit)
            return;

        TournamentLevel* levels = itr->second;

        if (levels->reqQuest && player->GetQuestStatus(levels->reqQuest) != QUEST_STATUS_COMPLETE)
            continue;

        if (levels->menuString)
            snprintf(gossipTextFormat, 100, sObjectMgr->GetAcoreStringForDBCLocale(levels->menuString), levels->level);
        else
            snprintf(gossipTextFormat, 100, "Start level: %u", levels->level);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, gossipTextFormat, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + levels->level);

        ++i;
    }
}

TournamentManager TournamentMgr;

class npc_tournaments_organizer : public CreatureScript
{
public:
    npc_tournaments_organizer() : CreatureScript("npc_tournaments_organizer") { }

    struct npc_gladiators_organizerAI : public ScriptedAI
    {
        npc_gladiators_organizerAI(Creature* creature) : ScriptedAI(creature)
        {
            TournamentMgr.setTournamentOrganizer(creature->GetOriginalEntry(), creature);
        }
    };

    CreatureAI* GetAI(Creature* creature) const
    {
        return new npc_gladiators_organizerAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature)
    {
        if (TournamentEnable)
            TournamentMgr.addGossip(creature, player);

        player->PlayerTalkClass->SendGossipMenu(player->GetGossipTextId(creature), creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action)
    {
        player->PlayerTalkClass->ClearMenus();

        if (TournamentEnable && action > GOSSIP_ACTION_INFO_DEF)
            TournamentMgr.start(creature->GetOriginalEntry(), action - GOSSIP_ACTION_INFO_DEF, player);

        CloseGossipMenuFor(player);
        return true;
    }
};

class ModTournamentsWorldScript : public WorldScript
{
public:
    ModTournamentsWorldScript() : WorldScript("ModTournamentsWorldScript") { }

    //void OnConfigLoad(bool /*reload*/)
    void OnStartup()
    {
        TournamentEnable = sConfigMgr->GetOption<bool>("Tournaments.Enable", true);
        TournamentDebug = sConfigMgr->GetOption<bool>("Tournaments.Debug", true);
        TournamentLadder = sConfigMgr->GetOption<bool>("Tournaments.Ladder", true);
        TournamentLadderText = sConfigMgr->GetOption<int32>("Tournaments.LadderText", 0);

        if (!TournamentEnable)
            return;

        TournamentMgr.load();
    }

    void OnUpdate(uint32 diff)
    {
        if (!TournamentEnable)
            return;

        TournamentMgr.update(diff);
    }
};

using namespace Acore::ChatCommands;

class ModTournamentsCommandScript : public CommandScript
{
public:
    ModTournamentsCommandScript() : CommandScript("ModTournamentsCommandScript") { }
    
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable gladiatorCommandTable =
        {
            { "reload",      HandleGladiatorsChatCommand,      SEC_ADMINISTRATOR,    Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "gladiator", gladiatorCommandTable }
        };
        return commandTable;
    }
    /*
     ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "gladiator",           HandleGladiatorsChatCommand,  SEC_ADMINISTRATOR,         Console::Yes }
        };
        return commandTable;
    }
    */
    static bool HandleGladiatorsChatCommand(ChatHandler* /*handler*/, char const* args)
    {
        if (!TournamentEnable || !*args)
            return false;

        std::string param = (char*)args;

        if (param == "reload")
        {
            TournamentMgr.load(true);
            return true;
        }

        return true;
    }
};

void AddModTournamentsScripts()
{
	  new ModTournamentsWorldScript();
    new ModTournamentsCommandScript();
    new npc_tournaments_organizer();
}


