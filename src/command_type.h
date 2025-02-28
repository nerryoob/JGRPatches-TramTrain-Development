/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file command_type.h Types related to commands. */

#ifndef COMMAND_TYPE_H
#define COMMAND_TYPE_H

#include "economy_type.h"
#include "strings_type.h"
#include "tile_type.h"
#include "core/span_type.hpp"
#include "3rdparty/optional/ottd_optional.h"
#include <string>

struct GRFFile;

enum CommandCostIntlFlags : uint8 {
	CCIF_NONE                     = 0,
	CCIF_SUCCESS                  = 1 << 0,
	CCIF_INLINE_EXTRA_MSG         = 1 << 1,
	CCIF_INLINE_TILE              = 1 << 2,
	CCIF_INLINE_RESULT            = 1 << 3,
};
DECLARE_ENUM_AS_BIT_SET(CommandCostIntlFlags)

/**
 * Common return value for all commands. Wraps the cost and
 * a possible error message/state together.
 */
class CommandCost {
	Money cost;                                 ///< The cost of this action
	ExpensesType expense_type;                  ///< the type of expence as shown on the finances view
	CommandCostIntlFlags flags;                 ///< Flags: see CommandCostIntlFlags
	StringID message;                           ///< Warning message for when success is unset
	union {
		uint32 result = 0;
		StringID extra_message;                 ///< Additional warning message for when success is unset
		TileIndex tile;
	} inl;

	struct CommandCostAuxiliaryData {
		uint32 textref_stack[16] = {};
		const GRFFile *textref_stack_grffile = nullptr; ///< NewGRF providing the #TextRefStack content.
		uint textref_stack_size = 0;                    ///< Number of uint32 values to put on the #TextRefStack for the error message.
		StringID extra_message = INVALID_STRING_ID;     ///< Additional warning message for when success is unset
		TileIndex tile = INVALID_TILE;
		uint32 result = 0;
	};
	std::unique_ptr<CommandCostAuxiliaryData> aux_data;

	void AllocAuxData();
	bool AddInlineData(CommandCostIntlFlags inline_flag);

public:
	/**
	 * Creates a command cost return with no cost and no error
	 */
	CommandCost() : cost(0), expense_type(INVALID_EXPENSES), flags(CCIF_SUCCESS), message(INVALID_STRING_ID) {}

	/**
	 * Creates a command return value the is failed with the given message
	 */
	explicit CommandCost(StringID msg) : cost(0), expense_type(INVALID_EXPENSES), flags(CCIF_NONE), message(msg) {}

	CommandCost(const CommandCost &other);
	CommandCost(CommandCost &&other) = default;
	CommandCost &operator=(const CommandCost &other);
	CommandCost &operator=(CommandCost &&other) = default;

	/**
	 * Creates a command return value the is failed with the given message
	 */
	static CommandCost DualErrorMessage(StringID msg, StringID extra_msg)
	{
		CommandCost cc(msg);
		cc.flags |= CCIF_INLINE_EXTRA_MSG;
		cc.inl.extra_message = extra_msg;
		return cc;
	}

	/**
	 * Creates a command cost with given expense type and start cost of 0
	 * @param ex_t the expense type
	 */
	explicit CommandCost(ExpensesType ex_t) : cost(0), expense_type(ex_t), flags(CCIF_SUCCESS), message(INVALID_STRING_ID) {}

	/**
	 * Creates a command return value with the given start cost and expense type
	 * @param ex_t the expense type
	 * @param cst the initial cost of this command
	 */
	CommandCost(ExpensesType ex_t, const Money &cst) : cost(cst), expense_type(ex_t), flags(CCIF_SUCCESS), message(INVALID_STRING_ID) {}


	/**
	 * Adds the given cost to the cost of the command.
	 * @param cost the cost to add
	 */
	inline void AddCost(const Money &cost)
	{
		this->cost += cost;
	}

	void AddCost(const CommandCost &cmd_cost);

	/**
	 * Multiplies the cost of the command by the given factor.
	 * @param factor factor to multiply the costs with
	 */
	inline void MultiplyCost(int factor)
	{
		this->cost *= factor;
	}

	/**
	 * The costs as made up to this moment
	 * @return the costs
	 */
	inline Money GetCost() const
	{
		return this->cost;
	}

	/**
	 * The expense type of the cost
	 * @return the expense type
	 */
	inline ExpensesType GetExpensesType() const
	{
		return this->expense_type;
	}

	/**
	 * Makes this #CommandCost behave like an error command.
	 * @param message The error message.
	 */
	void MakeError(StringID message)
	{
		assert(message != INVALID_STRING_ID);
		this->flags &= ~(CCIF_SUCCESS | CCIF_INLINE_EXTRA_MSG);
		this->message = message;
		if (this->aux_data) this->aux_data->extra_message = INVALID_STRING_ID;
	}

	void UseTextRefStack(const GRFFile *grffile, uint num_registers);

	/**
	 * Returns the NewGRF providing the #TextRefStack of the error message.
	 * @return the NewGRF.
	 */
	const GRFFile *GetTextRefStackGRF() const
	{
		return this->aux_data != nullptr ? this->aux_data->textref_stack_grffile : 0;
	}

	/**
	 * Returns the number of uint32 values for the #TextRefStack of the error message.
	 * @return number of uint32 values.
	 */
	uint GetTextRefStackSize() const
	{
		return this->aux_data != nullptr ? this->aux_data->textref_stack_size : 0;
	}

	/**
	 * Returns a pointer to the values for the #TextRefStack of the error message.
	 * @return uint32 values for the #TextRefStack
	 */
	const uint32 *GetTextRefStack() const
	{
		return this->aux_data != nullptr ? this->aux_data->textref_stack : nullptr;
	}

	/**
	 * Returns the error message of a command
	 * @return the error message, if succeeded #INVALID_STRING_ID
	 */
	StringID GetErrorMessage() const
	{
		if (this->Succeeded()) return INVALID_STRING_ID;
		return this->message;
	}

	/**
	 * Returns the extra error message of a command
	 * @return the extra error message, if succeeded #INVALID_STRING_ID
	 */
	StringID GetExtraErrorMessage() const
	{
		if (this->Succeeded()) return INVALID_STRING_ID;
		if (this->flags & CCIF_INLINE_EXTRA_MSG) return this->inl.extra_message;
		return this->aux_data != nullptr ? this->aux_data->extra_message : INVALID_STRING_ID;
	}

	/**
	 * Did this command succeed?
	 * @return true if and only if it succeeded
	 */
	inline bool Succeeded() const
	{
		return (this->flags & CCIF_SUCCESS);
	}

	/**
	 * Did this command fail?
	 * @return true if and only if it failed
	 */
	inline bool Failed() const
	{
		return !(this->flags & CCIF_SUCCESS);
	}

	/**
	 * @param cmd_msg optional failure string as passed to DoCommand
	 * @return an allocated string summarising the command result
	 */
	char *AllocSummaryMessage(StringID cmd_msg = 0) const;

	/**
	 * Write a string summarising the command result
	 * @param buf buffer to write to
	 * @param last last byte in buffer
	 * @param cmd_msg optional failure string as passed to DoCommand
	 * @return the number of bytes written
	 */
	int WriteSummaryMessage(char *buf, char *last, StringID cmd_msg = 0) const;

	bool IsSuccessWithMessage() const
	{
		return this->Succeeded() && this->message != INVALID_STRING_ID;
	}

	void MakeSuccessWithMessage()
	{
		assert(this->message != INVALID_STRING_ID);
		this->flags |= CCIF_SUCCESS;
	}

	CommandCost UnwrapSuccessWithMessage() const
	{
		assert(this->IsSuccessWithMessage());
		CommandCost res = *this;
		res.flags &= ~CCIF_SUCCESS;
		return res;
	}

	TileIndex GetTile() const
	{
		if (this->flags & CCIF_INLINE_TILE) return this->inl.tile;
		return this->aux_data != nullptr ? this->aux_data->tile : INVALID_TILE;
	}

	void SetTile(TileIndex tile);

	uint32 GetResultData() const
	{
		if (this->flags & CCIF_INLINE_RESULT) return this->inl.result;
		return this->aux_data != nullptr ? this->aux_data->result : 0;
	}

	void SetResultData(uint32 result);
};

/**
 * List of commands.
 *
 * This enum defines all possible commands which can be executed to the game
 * engine. Observing the game like the query-tool or checking the profit of a
 * vehicle don't result in a command which should be executed in the engine
 * nor send to the server in a network game.
 *
 * @see _command_proc_table
 */
enum Commands {
	CMD_BUILD_RAILROAD_TRACK,         ///< build a rail track
	CMD_REMOVE_RAILROAD_TRACK,        ///< remove a rail track
	CMD_BUILD_SINGLE_RAIL,            ///< build a single rail track
	CMD_REMOVE_SINGLE_RAIL,           ///< remove a single rail track
	CMD_LANDSCAPE_CLEAR,              ///< demolish a tile
	CMD_BUILD_BRIDGE,                 ///< build a bridge
	CMD_BUILD_RAIL_STATION,           ///< build a rail station
	CMD_BUILD_TRAIN_DEPOT,            ///< build a train depot
	CMD_BUILD_SIGNALS,                ///< build a signal
	CMD_REMOVE_SIGNALS,               ///< remove a signal
	CMD_TERRAFORM_LAND,               ///< terraform a tile
	CMD_BUILD_OBJECT,                 ///< build an object
	CMD_PURCHASE_LAND_AREA,           ///< purchase an area of landscape
	CMD_BUILD_OBJECT_AREA,            ///< build an area of objects
	CMD_BUILD_HOUSE,                  ///< build a house
	CMD_BUILD_TUNNEL,                 ///< build a tunnel

	CMD_REMOVE_FROM_RAIL_STATION,     ///< remove a (rectangle of) tiles from a rail station
	CMD_CONVERT_RAIL,                 ///< convert a rail type
	CMD_CONVERT_RAIL_TRACK,           ///< convert a rail type (track)

	CMD_BUILD_RAIL_WAYPOINT,          ///< build a waypoint
	CMD_BUILD_ROAD_WAYPOINT,          ///< build a road waypoint
	CMD_RENAME_WAYPOINT,              ///< rename a waypoint
	CMD_SET_WAYPOINT_LABEL_HIDDEN,    ///< set whether waypoint label is hidden
	CMD_REMOVE_FROM_RAIL_WAYPOINT,    ///< remove a (rectangle of) tiles from a rail waypoint

	CMD_BUILD_ROAD_STOP,              ///< build a road stop
	CMD_REMOVE_ROAD_STOP,             ///< remove a road stop
	CMD_BUILD_LONG_ROAD,              ///< build a complete road (not a "half" one)
	CMD_REMOVE_LONG_ROAD,             ///< remove a complete road (not a "half" one)
	CMD_BUILD_ROAD,                   ///< build a "half" road
	CMD_BUILD_ROAD_DEPOT,             ///< build a road depot
	CMD_CONVERT_ROAD,                 ///< convert a road type

	CMD_BUILD_AIRPORT,                ///< build an airport

	CMD_BUILD_DOCK,                   ///< build a dock

	CMD_BUILD_SHIP_DEPOT,             ///< build a ship depot
	CMD_BUILD_BUOY,                   ///< build a buoy

	CMD_PLANT_TREE,                   ///< plant a tree

	CMD_BUILD_VEHICLE,                ///< build a vehicle
	CMD_SELL_VEHICLE,                 ///< sell a vehicle
	CMD_REFIT_VEHICLE,                ///< refit the cargo space of a vehicle
	CMD_SEND_VEHICLE_TO_DEPOT,        ///< send a vehicle to a depot
	CMD_SET_VEHICLE_VISIBILITY,       ///< hide or unhide a vehicle in the build vehicle and autoreplace GUIs

	CMD_MOVE_RAIL_VEHICLE,            ///< move a rail vehicle (in the depot)
	CMD_FORCE_TRAIN_PROCEED,          ///< proceed a train to pass a red signal
	CMD_REVERSE_TRAIN_DIRECTION,      ///< turn a train around

	CMD_CLEAR_ORDER_BACKUP,           ///< clear the order backup of a given user/tile
	CMD_MODIFY_ORDER,                 ///< modify an order (like set full-load)
	CMD_SKIP_TO_ORDER,                ///< skip an order to the next of specific one
	CMD_DELETE_ORDER,                 ///< delete an order
	CMD_INSERT_ORDER,                 ///< insert a new order
	CMD_DUPLICATE_ORDER,              ///< duplicate an order
	CMD_MASS_CHANGE_ORDER,            ///< mass change the target of an order

	CMD_CHANGE_SERVICE_INT,           ///< change the server interval of a vehicle

	CMD_BUILD_INDUSTRY,               ///< build a new industry
	CMD_INDUSTRY_SET_FLAGS,           ///< change industry control flags
	CMD_INDUSTRY_SET_EXCLUSIVITY,     ///< change industry exclusive consumer/supplier
	CMD_INDUSTRY_SET_TEXT,            ///< change additional text for the industry

	CMD_SET_COMPANY_MANAGER_FACE,     ///< set the manager's face of the company
	CMD_SET_COMPANY_COLOUR,           ///< set the colour of the company

	CMD_INCREASE_LOAN,                ///< increase the loan from the bank
	CMD_DECREASE_LOAN,                ///< decrease the loan from the bank

	CMD_WANT_ENGINE_PREVIEW,          ///< confirm the preview of an engine
	CMD_ENGINE_CTRL,                  ///< control availability of the engine for companies

	CMD_SET_VEHICLE_UNIT_NUMBER,      ///< sets the unit number of a vehicle

	CMD_RENAME_VEHICLE,               ///< rename a whole vehicle
	CMD_RENAME_ENGINE,                ///< rename a engine (in the engine list)
	CMD_RENAME_COMPANY,               ///< change the company name
	CMD_RENAME_PRESIDENT,             ///< change the president name
	CMD_RENAME_STATION,               ///< rename a station
	CMD_RENAME_DEPOT,                 ///< rename a depot
	CMD_EXCHANGE_STATION_NAMES,       ///< exchange station names
	CMD_SET_STATION_CARGO_ALLOWED_SUPPLY, ///< set station cargo allowed supply

	CMD_PLACE_SIGN,                   ///< place a sign
	CMD_RENAME_SIGN,                  ///< rename a sign

	CMD_TURN_ROADVEH,                 ///< turn a road vehicle around

	CMD_PAUSE,                        ///< pause the game

	CMD_BUY_SHARE_IN_COMPANY,         ///< buy a share from a company
	CMD_SELL_SHARE_IN_COMPANY,        ///< sell a share from a company
	CMD_BUY_COMPANY,                  ///< buy a company which is bankrupt
	CMD_DECLINE_BUY_COMPANY,          ///< decline to buy a company which is bankrupt

	CMD_FOUND_TOWN,                   ///< found a town
	CMD_RENAME_TOWN,                  ///< rename a town
	CMD_RENAME_TOWN_NON_ADMIN,        ///< rename a town, non-admin command
	CMD_DO_TOWN_ACTION,               ///< do a action from the town detail window (like advertises or bribe)
	CMD_TOWN_SETTING_OVERRIDE,        ///< override a town setting
	CMD_TOWN_SETTING_OVERRIDE_NON_ADMIN, ///< override a town setting, non-admin command
	CMD_TOWN_CARGO_GOAL,              ///< set the goal of a cargo for a town
	CMD_TOWN_GROWTH_RATE,             ///< set the town growth rate
	CMD_TOWN_RATING,                  ///< set rating of a company in a town
	CMD_TOWN_SET_TEXT,                ///< set the custom text of a town
	CMD_EXPAND_TOWN,                  ///< expand a town
	CMD_DELETE_TOWN,                  ///< delete a town

	CMD_ORDER_REFIT,                  ///< change the refit information of an order (for "goto depot" )
	CMD_CLONE_ORDER,                  ///< clone (and share) an order
	CMD_CLEAR_AREA,                   ///< clear an area

	CMD_MONEY_CHEAT,                  ///< do the money cheat
	CMD_MONEY_CHEAT_ADMIN,            ///< do the money cheat (admin mode)
	CMD_CHANGE_BANK_BALANCE,          ///< change bank balance to charge costs or give money from a GS
	CMD_CHEAT_SETTING,                ///< change a cheat setting
	CMD_BUILD_CANAL,                  ///< build a canal

	CMD_CREATE_SUBSIDY,               ///< create a new subsidy
	CMD_COMPANY_CTRL,                 ///< used in multiplayer to create a new companies etc.
	CMD_CUSTOM_NEWS_ITEM,             ///< create a custom news message
	CMD_CREATE_GOAL,                  ///< create a new goal
	CMD_REMOVE_GOAL,                  ///< remove a goal
	CMD_SET_GOAL_TEXT,                ///< update goal text of a goal
	CMD_SET_GOAL_PROGRESS,            ///< update goal progress text of a goal
	CMD_SET_GOAL_COMPLETED,           ///< update goal completed status of a goal
	CMD_GOAL_QUESTION,                ///< ask a goal related question
	CMD_GOAL_QUESTION_ANSWER,         ///< answer(s) to CMD_GOAL_QUESTION
	CMD_CREATE_STORY_PAGE,            ///< create a new story page
	CMD_CREATE_STORY_PAGE_ELEMENT,    ///< create a new story page element
	CMD_UPDATE_STORY_PAGE_ELEMENT,    ///< update a story page element
	CMD_SET_STORY_PAGE_TITLE,         ///< update title of a story page
	CMD_SET_STORY_PAGE_DATE,          ///< update date of a story page
	CMD_SHOW_STORY_PAGE,              ///< show a story page
	CMD_REMOVE_STORY_PAGE,            ///< remove a story page
	CMD_REMOVE_STORY_PAGE_ELEMENT,    ///< remove a story page element
	CMD_SCROLL_VIEWPORT,              ///< scroll main viewport of players
	CMD_STORY_PAGE_BUTTON,            ///< selection via story page button

	CMD_LEVEL_LAND,                   ///< level land

	CMD_BUILD_LOCK,                   ///< build a lock

	CMD_BUILD_SIGNAL_TRACK,           ///< add signals along a track (by dragging)
	CMD_REMOVE_SIGNAL_TRACK,          ///< remove signals along a track (by dragging)

	CMD_GIVE_MONEY,                   ///< give money to another company
	CMD_CHANGE_SETTING,               ///< change a setting
	CMD_CHANGE_COMPANY_SETTING,       ///< change a company setting

	CMD_SET_AUTOREPLACE,              ///< set an autoreplace entry

	CMD_TOGGLE_REUSE_DEPOT_VEHICLES,  ///< toggle 'reuse depot vehicles' on template
	CMD_TOGGLE_KEEP_REMAINING_VEHICLES, ///< toggle 'keep remaining vehicles' on template
	CMD_TOGGLE_REFIT_AS_TEMPLATE,     ///< toggle 'refit as template' on template
	CMD_TOGGLE_TMPL_REPLACE_OLD_ONLY, ///< toggle 'replace old vehicles only' on template
	CMD_RENAME_TMPL_REPLACE,          ///< rename a template

	CMD_VIRTUAL_TRAIN_FROM_TEMPLATE_VEHICLE, ///< Creates a virtual train from a template
	CMD_VIRTUAL_TRAIN_FROM_TRAIN,     ///< Creates a virtual train from a regular train
	CMD_DELETE_VIRTUAL_TRAIN,         ///< Delete a virtual train
	CMD_BUILD_VIRTUAL_RAIL_VEHICLE,   ///< Build a virtual train
	CMD_REPLACE_TEMPLATE_VEHICLE,     ///< Replace a template vehicle with another one based on a virtual train
	CMD_MOVE_VIRTUAL_RAIL_VEHICLE,    ///< Move a virtual rail vehicle
	CMD_SELL_VIRTUAL_VEHICLE,         ///< Sell a virtual vehicle

	CMD_CLONE_TEMPLATE_VEHICLE_FROM_TRAIN, ///< clone a train and create a new template vehicle based on it
	CMD_DELETE_TEMPLATE_VEHICLE,      ///< delete a template vehicle

	CMD_ISSUE_TEMPLATE_REPLACEMENT,   ///< issue a template replacement for a vehicle group
	CMD_DELETE_TEMPLATE_REPLACEMENT,  ///< delete a template replacement from a vehicle group

	CMD_CLONE_VEHICLE,                ///< clone a vehicle
	CMD_CLONE_VEHICLE_FROM_TEMPLATE,  ///< clone a vehicle from a template
	CMD_START_STOP_VEHICLE,           ///< start or stop a vehicle
	CMD_MASS_START_STOP,              ///< start/stop all vehicles (in a depot)
	CMD_AUTOREPLACE_VEHICLE,          ///< replace/renew a vehicle while it is in a depot
	CMD_TEMPLATE_REPLACE_VEHICLE,     ///< template replace a vehicle while it is in a depot
	CMD_DEPOT_SELL_ALL_VEHICLES,      ///< sell all vehicles which are in a given depot
	CMD_DEPOT_MASS_AUTOREPLACE,       ///< force the autoreplace to take action in a given depot

	CMD_CREATE_GROUP,                 ///< create a new group
	CMD_DELETE_GROUP,                 ///< delete a group
	CMD_ALTER_GROUP,                  ///< alter a group
	CMD_CREATE_GROUP_FROM_LIST,       ///< create and rename a new group from a vehicle list
	CMD_ADD_VEHICLE_GROUP,            ///< add a vehicle to a group
	CMD_ADD_SHARED_VEHICLE_GROUP,     ///< add all other shared vehicles to a group which are missing
	CMD_REMOVE_ALL_VEHICLES_GROUP,    ///< remove all vehicles from a group
	CMD_SET_GROUP_FLAG,               ///< set/clear a flag for a group
	CMD_SET_GROUP_LIVERY,             ///< set the livery for a group

	CMD_MOVE_ORDER,                   ///< move an order
	CMD_REVERSE_ORDER_LIST,           ///< reverse order list
	CMD_CHANGE_TIMETABLE,             ///< change the timetable for a vehicle
	CMD_BULK_CHANGE_TIMETABLE,        ///< change the timetable for all orders of a vehicle
	CMD_SET_VEHICLE_ON_TIME,          ///< set the vehicle on time feature (timetable)
	CMD_AUTOFILL_TIMETABLE,           ///< autofill the timetable
	CMD_AUTOMATE_TIMETABLE,           ///< automate the timetable
	CMD_TIMETABLE_SEPARATION,         ///< auto timetable separation
	CMD_SET_TIMETABLE_START,          ///< set the date that a timetable should start

	CMD_OPEN_CLOSE_AIRPORT,           ///< open/close an airport to incoming aircraft

	CMD_CREATE_LEAGUE_TABLE,               ///< create a new league table
	CMD_CREATE_LEAGUE_TABLE_ELEMENT,       ///< create a new element in a league table
	CMD_UPDATE_LEAGUE_TABLE_ELEMENT_DATA,  ///< update the data fields of a league table element
	CMD_UPDATE_LEAGUE_TABLE_ELEMENT_SCORE, ///< update the score of a league table element
	CMD_REMOVE_LEAGUE_TABLE_ELEMENT,       ///< remove a league table element

	CMD_PROGRAM_TRACERESTRICT_SIGNAL, ///< modify a signal tracerestrict program
	CMD_CREATE_TRACERESTRICT_SLOT,    ///< create a tracerestrict slot
	CMD_ALTER_TRACERESTRICT_SLOT,     ///< alter a tracerestrict slot
	CMD_DELETE_TRACERESTRICT_SLOT,    ///< delete a tracerestrict slot
	CMD_ADD_VEHICLE_TRACERESTRICT_SLOT,    ///< add a vehicle to a tracerestrict slot
	CMD_REMOVE_VEHICLE_TRACERESTRICT_SLOT, ///< remove a vehicle from a tracerestrict slot
	CMD_CREATE_TRACERESTRICT_COUNTER, ///< create a tracerestrict counter
	CMD_ALTER_TRACERESTRICT_COUNTER,  ///< alter a tracerestrict counter
	CMD_DELETE_TRACERESTRICT_COUNTER, ///< delete a tracerestrict counter

	CMD_INSERT_SIGNAL_INSTRUCTION,    ///< insert a signal instruction
	CMD_MODIFY_SIGNAL_INSTRUCTION,    ///< modifies a signal instruction
	CMD_REMOVE_SIGNAL_INSTRUCTION,    ///< removes a signal instruction
	CMD_SIGNAL_PROGRAM_MGMT,          ///< removes a signal program management command

	CMD_SCHEDULED_DISPATCH,                     ///< scheduled dispatch start
	CMD_SCHEDULED_DISPATCH_ADD,                 ///< scheduled dispatch add
	CMD_SCHEDULED_DISPATCH_REMOVE,              ///< scheduled dispatch remove
	CMD_SCHEDULED_DISPATCH_SET_DURATION,        ///< scheduled dispatch set schedule duration
	CMD_SCHEDULED_DISPATCH_SET_START_DATE,      ///< scheduled dispatch set start date
	CMD_SCHEDULED_DISPATCH_SET_DELAY,           ///< scheduled dispatch set maximum allow delay
	CMD_SCHEDULED_DISPATCH_RESET_LAST_DISPATCH, ///< scheduled dispatch reset last dispatch date
	CMD_SCHEDULED_DISPATCH_CLEAR,               ///< scheduled dispatch clear schedule
	CMD_SCHEDULED_DISPATCH_ADD_NEW_SCHEDULE,    ///< scheduled dispatch add new schedule
	CMD_SCHEDULED_DISPATCH_REMOVE_SCHEDULE,     ///< scheduled dispatch remove schedule
	CMD_SCHEDULED_DISPATCH_RENAME_SCHEDULE,     ///< scheduled dispatch rename schedule
	CMD_SCHEDULED_DISPATCH_DUPLICATE_SCHEDULE,  ///< scheduled dispatch duplicate schedule
	CMD_SCHEDULED_DISPATCH_APPEND_VEHICLE_SCHEDULE, ///< scheduled dispatch append schedules from another vehicle
	CMD_SCHEDULED_DISPATCH_ADJUST,              ///< scheduled dispatch adjust time offsets in schedule

	CMD_ADD_PLAN,
	CMD_ADD_PLAN_LINE,
	CMD_REMOVE_PLAN,
	CMD_REMOVE_PLAN_LINE,
	CMD_CHANGE_PLAN_VISIBILITY,
	CMD_CHANGE_PLAN_COLOUR,
	CMD_RENAME_PLAN,

	CMD_DESYNC_CHECK,                 ///< Force desync checks to be run

	CMD_END,                          ///< Must ALWAYS be on the end of this list!! (period)
};

/**
 * List of flags for a command.
 *
 * This enums defines some flags which can be used for the commands.
 */
enum DoCommandFlag {
	DC_NONE                  = 0x000, ///< no flag is set
	DC_EXEC                  = 0x001, ///< execute the given command
	DC_AUTO                  = 0x002, ///< don't allow building on structures
	DC_QUERY_COST            = 0x004, ///< query cost only,  don't build.
	DC_NO_WATER              = 0x008, ///< don't allow building on water
	// 0x010 is unused
	DC_NO_TEST_TOWN_RATING   = 0x020, ///< town rating does not disallow you from building
	DC_BANKRUPT              = 0x040, ///< company bankrupts, skip money check, skip vehicle on tile check in some cases
	DC_AUTOREPLACE           = 0x080, ///< autoreplace/autorenew is in progress, this shall disable vehicle limits when building, and ignore certain restrictions when undoing things (like vehicle attach callback)
	DC_NO_CARGO_CAP_CHECK    = 0x100, ///< when autoreplace/autorenew is in progress, this shall prevent truncating the amount of cargo in the vehicle to prevent testing the command to remove cargo
	DC_ALL_TILES             = 0x200, ///< allow this command also on MP_VOID tiles
	DC_NO_MODIFY_TOWN_RATING = 0x400, ///< do not change town rating
	DC_FORCE_CLEAR_TILE      = 0x800, ///< do not only remove the object on the tile, but also clear any water left on it
	DC_ALLOW_REMOVE_WATER    = 0x1000,///< always allow removing water
	DC_TOWN                  = 0x2000,///< town operation
};
DECLARE_ENUM_AS_BIT_SET(DoCommandFlag)

/**
 * Used to combine a StringID with the command.
 *
 * This macro can be used to add a StringID (the error message to show) on a command-id
 * (CMD_xxx). Use the binary or-operator "|" to combine the command with the result from
 * this macro.
 *
 * @param x The StringID to combine with a command-id
 */
#define CMD_MSG(x) ((x) << 16)

/**
 * Defines some flags.
 *
 * This enumeration defines some flags which are binary-or'ed on a command.
 */
enum FlaggedCommands {
	CMD_NETWORK_COMMAND       = 0x0100, ///< execute the command without sending it on the network
	CMD_NO_SHIFT_ESTIMATE     = 0x0200, ///< do not check shift key state for whether to estimate command
	CMD_FLAGS_MASK            = 0xFF00, ///< mask for all command flags
	CMD_ID_MASK               = 0x00FF, ///< mask for the command ID
};

static_assert(CMD_END <= CMD_ID_MASK + 1);

/**
 * Command flags for the command table _command_proc_table.
 *
 * This enumeration defines flags for the _command_proc_table.
 */
enum CommandFlags {
	CMD_SERVER    =  0x001, ///< the command can only be initiated by the server
	CMD_SPECTATOR =  0x002, ///< the command may be initiated by a spectator
	CMD_OFFLINE   =  0x004, ///< the command cannot be executed in a multiplayer game; single-player only
	CMD_AUTO      =  0x008, ///< set the DC_AUTO flag on this command
	CMD_ALL_TILES =  0x010, ///< allow this command also on MP_VOID tiles
	CMD_NO_TEST   =  0x020, ///< the command's output may differ between test and execute due to town rating changes etc.
	CMD_NO_WATER  =  0x040, ///< set the DC_NO_WATER flag on this command
	CMD_CLIENT_ID =  0x080, ///< set p2 with the ClientID of the sending client.
	CMD_DEITY     =  0x100, ///< the command may be executed by COMPANY_DEITY
	CMD_STR_CTRL  =  0x200, ///< the command's string may contain control strings
	CMD_NO_EST    =  0x400, ///< the command is never estimated.
	CMD_PROCEX    =  0x800, ///< the command proc function has extended parameters
	CMD_SERVER_NS = 0x1000, ///< the command can only be initiated by the server (this is not executed in spectator mode)
	CMD_LOG_AUX   = 0x2000, ///< the command should be logged in the auxiliary log instead of the main log
	CMD_P1_TILE   = 0x4000, ///< use p1 for money text and error tile
};
DECLARE_ENUM_AS_BIT_SET(CommandFlags)

/** Types of commands we have. */
enum CommandType {
	CMDT_LANDSCAPE_CONSTRUCTION, ///< Construction and destruction of objects on the map.
	CMDT_VEHICLE_CONSTRUCTION,   ///< Construction, modification (incl. refit) and destruction of vehicles.
	CMDT_MONEY_MANAGEMENT,       ///< Management of money, i.e. loans and shares.
	CMDT_VEHICLE_MANAGEMENT,     ///< Stopping, starting, sending to depot, turning around, replace orders etc.
	CMDT_ROUTE_MANAGEMENT,       ///< Modifications to route management (orders, groups, etc).
	CMDT_OTHER_MANAGEMENT,       ///< Renaming stuff, changing company colours, placing signs, etc.
	CMDT_COMPANY_SETTING,        ///< Changing settings related to a company.
	CMDT_SERVER_SETTING,         ///< Pausing/removing companies/server settings.
	CMDT_CHEAT,                  ///< A cheat of some sorts.

	CMDT_END,                    ///< Magic end marker.
};

/** Different command pause levels. */
enum CommandPauseLevel {
	CMDPL_NO_ACTIONS,      ///< No user actions may be executed.
	CMDPL_NO_CONSTRUCTION, ///< No construction actions may be executed.
	CMDPL_NO_LANDSCAPING,  ///< No landscaping actions may be executed.
	CMDPL_ALL_ACTIONS,     ///< All actions may be executed.
};

struct CommandAuxiliaryBase;

/**
 * Defines the callback type for all command handler functions.
 *
 * This type defines the function header for all functions which handles a CMD_* command.
 * A command handler use the parameters to act according to the meaning of the command.
 * The tile parameter defines the tile to perform an action on.
 * The flag parameter is filled with flags from the DC_* enumeration. The parameters
 * p1 and p2 are filled with parameters for the command like "which road type", "which
 * order" or "direction". Each function should mentioned in there doxygen comments
 * the usage of these parameters.
 *
 * @param tile The tile to apply a command on
 * @param flags Flags for the command, from the DC_* enumeration
 * @param p1 Additional data for the command
 * @param p2 Additional data for the command
 * @param text Additional text
 * @return The CommandCost of the command, which can be succeeded or failed.
 */
typedef CommandCost CommandProc(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text);
typedef CommandCost CommandProcEx(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, uint64 p3, const char *text, const CommandAuxiliaryBase *aux_data);

/**
 * Define a command with the flags which belongs to it.
 *
 * This struct connect a command handler function with the flags created with
 * the #CMD_AUTO, #CMD_OFFLINE and #CMD_SERVER values.
 */
struct Command {
	union {
		CommandProc *proc;      ///< The procedure to actually execute
		CommandProcEx *procex;  ///< The procedure to actually execute, extended parameters
	};
	const char *name;   ///< A human readable name for the procedure
	CommandFlags flags; ///< The (command) flags to that apply to this command
	CommandType type;   ///< The type of command.

	Command(CommandProc *proc, const char *name, CommandFlags flags, CommandType type)
			: proc(proc), name(name), flags(flags & ~CMD_PROCEX), type(type) {}
	Command(CommandProcEx *procex, const char *name, CommandFlags flags, CommandType type)
			: procex(procex), name(name), flags(flags | CMD_PROCEX), type(type) {}

	inline CommandCost Execute(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, uint64 p3, const char *text, const CommandAuxiliaryBase *aux_data) const {
		if (this->flags & CMD_PROCEX) {
			return this->procex(tile, flags, p1, p2, p3, text, aux_data);
		} else {
			return this->proc(tile, flags, p1, p2, text);
		}
	}
};

/**
 * Define a callback function for the client, after the command is finished.
 *
 * Functions of this type are called after the command is finished. The parameters
 * are from the #CommandProc callback type. The boolean parameter indicates if the
 * command succeeded or failed.
 *
 * @param result The result of the executed command
 * @param tile The tile of the command action
 * @param p1 Additional data of the command
 * @param p2 Additional data of the command
 * @param p3 Additional data of the command
 * @see CommandProc
 */
typedef void CommandCallback(const CommandCost &result, TileIndex tile, uint32 p1, uint32 p2, uint64 p3, uint32 cmd);

#define MAX_CMD_TEXT_LENGTH 32000

struct CommandSerialisationBuffer;

struct CommandAuxiliaryBase {
	virtual ~CommandAuxiliaryBase() {}

	virtual CommandAuxiliaryBase *Clone() const = 0;

	virtual opt::optional<span<const uint8>> GetDeserialisationSrc() const = 0;

	virtual void Serialise(CommandSerialisationBuffer &buffer) const = 0;
};

struct CommandAuxiliaryPtr : public std::unique_ptr<CommandAuxiliaryBase>
{
	using std::unique_ptr<CommandAuxiliaryBase>::unique_ptr;

	CommandAuxiliaryPtr() {};

	CommandAuxiliaryPtr(const CommandAuxiliaryPtr &other) :
			std::unique_ptr<CommandAuxiliaryBase>(CommandAuxiliaryPtr::Clone(other)) {}

	CommandAuxiliaryPtr& operator=(const CommandAuxiliaryPtr &other)
	{
		this->reset(CommandAuxiliaryPtr::Clone(other));
		return *this;
	}

private:
	static CommandAuxiliaryBase *Clone(const CommandAuxiliaryPtr &other)
	{
		return other != nullptr ? other->Clone() : nullptr;
	}
};

/**
 * Structure for buffering the build command when selecting a station to join.
 */
struct CommandContainer {
	TileIndex tile;                  ///< tile command being executed on.
	uint32 p1;                       ///< parameter p1.
	uint32 p2;                       ///< parameter p2.
	uint32 cmd;                      ///< command being executed.
	uint64 p3;                       ///< parameter p3. (here for alignment)
	CommandCallback *callback;       ///< any callback function executed upon successful completion of the command.
	std::string text;                ///< possible text sent for name changes etc.
	CommandAuxiliaryPtr aux_data;    ///< Auxiliary command data
};

inline CommandContainer NewCommandContainerBasic(TileIndex tile, uint32 p1, uint32 p2, uint32 cmd, CommandCallback *callback = nullptr)
{
	return { tile, p1, p2, cmd, 0, callback, {}, nullptr };
}

#endif /* COMMAND_TYPE_H */
