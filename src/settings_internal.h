/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file settings_internal.h Functions and types used internally for the settings configurations. */

#ifndef SETTINGS_INTERNAL_H
#define SETTINGS_INTERNAL_H

#include "saveload/saveload_types.h"

enum SettingFlag : uint32 {
	SF_NONE = 0,
	SF_GUI_0_IS_SPECIAL        = 1 <<  0, ///< A value of zero is possible and has a custom string (the one after "strval").
	SF_GUI_NEGATIVE_IS_SPECIAL = 1 <<  1, ///< A negative value has another string (the one after "strval").
	SF_GUI_DROPDOWN            = 1 <<  2, ///< The value represents a limited number of string-options (internally integer) presented as dropdown.
	SF_GUI_CURRENCY            = 1 <<  3, ///< The number represents money, so when reading value multiply by exchange rate.
	SF_NETWORK_ONLY            = 1 <<  4, ///< This setting only applies to network games.
	SF_NO_NETWORK              = 1 <<  5, ///< This setting does not apply to network games; it may not be changed during the game.
	SF_NEWGAME_ONLY            = 1 <<  6, ///< This setting cannot be changed in a game.
	SF_SCENEDIT_TOO            = 1 <<  7, ///< This setting can be changed in the scenario editor (only makes sense when SF_NEWGAME_ONLY is set).
	SF_SCENEDIT_ONLY           = 1 <<  8, ///< This setting can only be changed in the scenario editor.
	SF_PER_COMPANY             = 1 <<  9, ///< This setting can be different for each company (saved in company struct).
	SF_NOT_IN_SAVE             = 1 << 10, ///< Do not save with savegame, basically client-based.
	SF_NOT_IN_CONFIG           = 1 << 11, ///< Do not save to config file.
	SF_NO_NETWORK_SYNC         = 1 << 12, ///< Do not synchronize over network (but it is saved if SF_NOT_IN_SAVE is not set).
	SF_DECIMAL1                = 1 << 13, ///< display a decimal representation of the setting value divided by 10
	SF_ENUM                    = 1 << 14, ///< the setting can take one of the values given by an array of struct SettingDescEnumEntry
	SF_NO_NEWGAME              = 1 << 15, ///< the setting does not apply and is not shown in a new game context
	SF_DEC1SCALE               = 1 << 16, ///< also display a float representation of the scale of a decimal1 scale parameter
	SF_RUN_CALLBACKS_ON_PARSE  = 1 << 17, ///< run callbacks when parsing from config file
	SF_GUI_VELOCITY            = 1 << 18, ///< setting value is a velocity
	SF_GUI_ADVISE_DEFAULT      = 1 << 19, ///< Advise the user to leave this setting at its default value
	SF_ENUM_PRE_CB_VALIDATE    = 1 << 20, ///< Call the pre_check callback for enum incoming value validation
	SF_CONVERT_BOOL_TO_INT     = 1 << 21, ///< Accept a boolean value when loading an int-type setting from the config file
	SF_ENABLE_UPSTREAM_LOAD    = 1 << 22, ///< Enable loading from upstream mode savegames even when patx_name is set
};
DECLARE_ENUM_AS_BIT_SET(SettingFlag)

/**
 * A SettingCategory defines a grouping of the settings.
 * The group #SC_BASIC is intended for settings which also a novice player would like to change and is able to understand.
 * The group #SC_ADVANCED is intended for settings which an experienced player would like to use. This is the case for most settings.
 * Finally #SC_EXPERT settings only few people want to see in rare cases.
 * The grouping is meant to be inclusive, i.e. all settings in #SC_BASIC also will be included
 * in the set of settings in #SC_ADVANCED. The group #SC_EXPERT contains all settings.
 */
enum SettingCategory {
	SC_NONE = 0,

	/* Filters for the list */
	SC_BASIC_LIST      = 1 << 0,    ///< Settings displayed in the list of basic settings.
	SC_ADVANCED_LIST   = 1 << 1,    ///< Settings displayed in the list of advanced settings.
	SC_EXPERT_LIST     = 1 << 2,    ///< Settings displayed in the list of expert settings.

	/* Setting classification */
	SC_BASIC           = SC_BASIC_LIST | SC_ADVANCED_LIST | SC_EXPERT_LIST,  ///< Basic settings are part of all lists.
	SC_ADVANCED        = SC_ADVANCED_LIST | SC_EXPERT_LIST,                  ///< Advanced settings are part of advanced and expert list.
	SC_EXPERT          = SC_EXPERT_LIST,                                     ///< Expert settings can only be seen in the expert list.

	SC_END,
};

/**
 * Type of settings for filtering.
 */
enum SettingType {
	ST_GAME,      ///< Game setting.
	ST_COMPANY,   ///< Company setting.
	ST_CLIENT,    ///< Client setting.

	ST_ALL,       ///< Used in setting filter to match all types.
};

enum SettingOnGuiCtrlType {
	SOGCT_DESCRIPTION_TEXT,   ///< Description text callback
	SOGCT_GUI_DROPDOWN_ORDER, ///< SF_GUI_DROPDOWN reordering callback
	SOGCT_CFG_NAME,           ///< Config file name override
	SOGCT_CFG_FALLBACK_NAME,  ///< Config file name within group fallback
};

struct SettingOnGuiCtrlData {
	SettingOnGuiCtrlType type;
	StringID text;
	int val;
	const char *str = nullptr;
};

struct IniItem;
typedef bool OnGuiCtrl(SettingOnGuiCtrlData &data); ///< callback prototype for GUI operations
typedef int64 OnXrefValueConvert(int64 val); ///< callback prototype for xref value conversion

/** The last entry in an array of struct SettingDescEnumEntry must use STR_NULL. */
struct SettingDescEnumEntry {
	int32 val;
	StringID str;
};

struct SettingsXref {
	const char *target;
	OnXrefValueConvert *conv;

	SettingsXref() : target(nullptr), conv(nullptr) {}
	SettingsXref(const char *target_, OnXrefValueConvert *conv_) : target(target_), conv(conv_) {}
};

/** Properties of config file settings. */
struct SettingDesc {
	struct XrefContructorTag {};
	SettingDesc(const SaveLoad &save, const char *name, SettingFlag flags, OnGuiCtrl *guiproc, bool startup, const char *patx_name) :
		name(name), flags(flags), guiproc(guiproc), startup(startup), save(save), patx_name(patx_name) {}
	SettingDesc(XrefContructorTag tag, SaveLoad save, SettingsXref xref) :
		name(nullptr), flags(SF_NONE), guiproc(nullptr), startup(false), save(save), patx_name(nullptr), xref(xref) {}
	virtual ~SettingDesc() {}

	const char *name;       ///< Name of the setting. Used in configuration file and for console
	SettingFlag flags;      ///< Handles how a setting would show up in the GUI (text/currency, etc.)
	OnGuiCtrl *guiproc;     ///< Callback procedure for GUI operations
	bool startup;           ///< Setting has to be loaded directly at startup?
	SaveLoad save;          ///< Internal structure (going to savegame, parts to config)

	const char *patx_name;  ///< Name to save/load setting from in PATX chunk, if nullptr save/load from PATS chunk as normal
	SettingsXref xref;      ///< Details of SettingDesc to use instead of the contents of this one, useful for loading legacy savegames, if target field nullptr save/load as normal

	bool IsEditable(bool do_command = false) const;
	SettingType GetType() const;

	/**
	 * Check whether this setting is an integer type setting.
	 * @return True when the underlying type is an integer.
	 */
	virtual bool IsIntSetting() const { return false; }

	/**
	 * Check whether this setting is an string type setting.
	 * @return True when the underlying type is a string.
	 */
	virtual bool IsStringSetting() const { return false; }

	const struct IntSettingDesc *AsIntSetting() const;
	const struct StringSettingDesc *AsStringSetting() const;

	/**
	 * Format the value of the setting associated with this object.
	 * @param buf The before of the buffer to format into.
	 * @param last The end of the buffer to format into.
	 * @param object The object the setting is in.
	 */
	virtual void FormatValue(char *buf, const char *last, const void *object) const = 0;

	/**
	 * Parse/read the value from the Ini item into the setting associated with this object.
	 * @param item The Ini item with the content of this setting.
	 * @param object The object the setting is in.
	 */
	virtual void ParseValue(const IniItem *item, void *object) const = 0;

	/**
	 * Check whether the value in the Ini item is the same as is saved in this setting in the object.
	 * It might be that determining whether the value is the same is way more expensive than just
	 * writing the value. In those cases this function may unconditionally return false even though
	 * the value might be the same as in the Ini item.
	 * @param item The Ini item with the content of this setting.
	 * @param object The object the setting is in.
	 * @return True if the value is definitely the same (might be false when the same).
	 */
	virtual bool IsSameValue(const IniItem *item, void *object) const = 0;
};

/** Base integer type, including boolean, settings. Only these are shown in the settings UI. */
struct IntSettingDesc : SettingDesc {
	/**
	 * A check to be performed before the setting gets changed. The passed integer may be
	 * changed by the check if that is important, for example to remove some unwanted bit.
	 * The return value denotes whether the value, potentially after the changes,
	 * is allowed to be used/set in the configuration.
	 * @param value The prospective new value for the setting.
	 * @return True when the setting is accepted.
	 */
	typedef bool PreChangeCheck(int32 &value);
	/**
	 * A callback to denote that a setting has been changed.
	 * @param The new value for the setting.
	 */
	typedef void PostChangeCallback(int32 value);

	IntSettingDesc(const SaveLoad &save, const char *name, SettingFlag flags, OnGuiCtrl *guiproc, bool startup, const char *patx_name, int32 def,
			int32 min, uint32 max, int32 interval, StringID str, StringID str_help, StringID str_val,
			SettingCategory cat, PreChangeCheck pre_check, PostChangeCallback post_callback, const SettingDescEnumEntry *enumlist) :
		SettingDesc(save, name, flags, guiproc, startup, patx_name), def(def), min(min), max(max), interval(interval),
			str(str), str_help(str_help), str_val(str_val), cat(cat), pre_check(pre_check),
			post_callback(post_callback), enumlist(enumlist) {}
	virtual ~IntSettingDesc() {}

	int32 def;              ///< default value given when none is present
	int32 min;              ///< minimum values
	uint32 max;             ///< maximum values
	int32 interval;         ///< the interval to use between settings in the 'settings' window. If interval is '0' the interval is dynamically determined
	StringID str;           ///< (translated) string with descriptive text; gui and console
	StringID str_help;      ///< (Translated) string with help text; gui only.
	StringID str_val;       ///< (Translated) first string describing the value.
	SettingCategory cat;    ///< assigned categories of the setting
	PreChangeCheck *pre_check;         ///< Callback to check for the validity of the setting.
	PostChangeCallback *post_callback; ///< Callback when the setting has been changed.

	const SettingDescEnumEntry *enumlist; ///< For SF_ENUM. The last entry must use STR_NULL

	/**
	 * Check whether this setting is a boolean type setting.
	 * @return True when the underlying type is an integer.
	 */
	virtual bool IsBoolSetting() const { return false; }
	bool IsIntSetting() const override { return true; }

	void ChangeValue(const void *object, int32 newvalue) const;
	void MakeValueValidAndWrite(const void *object, int32 value) const;

	virtual size_t ParseValue(const char *str) const;
	void FormatValue(char *buf, const char *last, const void *object) const override;
	virtual void FormatIntValue(char *buf, const char *last, uint32 value) const;
	void ParseValue(const IniItem *item, void *object) const override;
	bool IsSameValue(const IniItem *item, void *object) const override;
	int32 Read(const void *object) const;

private:
	void MakeValueValid(int32 &value) const;
	void Write(const void *object, int32 value) const;
};

/** Boolean setting. */
struct BoolSettingDesc : IntSettingDesc {
	BoolSettingDesc(const SaveLoad &save, const char *name, SettingFlag flags, OnGuiCtrl *guiproc, bool startup, const char *patx_name, bool def,
		StringID str, StringID str_help, StringID str_val, SettingCategory cat,
		PreChangeCheck pre_check, PostChangeCallback post_callback) :
		IntSettingDesc(save, name, flags, guiproc, startup, patx_name, def, 0, 1, 0, str, str_help, str_val, cat, pre_check, post_callback, nullptr) {}
	virtual ~BoolSettingDesc() {}

	bool IsBoolSetting() const override { return true; }
	size_t ParseValue(const char *str) const override;
	void FormatIntValue(char *buf, const char *last, uint32 value) const override;
};

/** One of many setting. */
struct OneOfManySettingDesc : IntSettingDesc {
	typedef size_t OnConvert(const char *value); ///< callback prototype for conversion error

	OneOfManySettingDesc(const SaveLoad &save, const char *name, SettingFlag flags, OnGuiCtrl *guiproc, bool startup, const char *patx_name,
		int32 def, int32 max, StringID str, StringID str_help, StringID str_val, SettingCategory cat,
		PreChangeCheck pre_check, PostChangeCallback post_callback,
		std::initializer_list<const char *> many, OnConvert *many_cnvt) :
		IntSettingDesc(save, name, flags, guiproc, startup, patx_name, def, 0, max, 0, str, str_help, str_val, cat, pre_check, post_callback, nullptr), many_cnvt(many_cnvt)
	{
		for (auto one : many) this->many.push_back(one);
	}

	virtual ~OneOfManySettingDesc() {}

	std::vector<std::string> many; ///< possible values for this type
	OnConvert *many_cnvt;          ///< callback procedure when loading value mechanism fails

	static size_t ParseSingleValue(const char *str, size_t len, const std::vector<std::string> &many);
	char *FormatSingleValue(char *buf, const char *last, uint id) const;

	size_t ParseValue(const char *str) const override;
	void FormatIntValue(char *buf, const char *last, uint32 value) const override;
};

/** Many of many setting. */
struct ManyOfManySettingDesc : OneOfManySettingDesc {
	ManyOfManySettingDesc(const SaveLoad &save, const char *name, SettingFlag flags, OnGuiCtrl *guiproc, bool startup, const char *patx_name,
		int32 def, StringID str, StringID str_help, StringID str_val, SettingCategory cat,
		PreChangeCheck pre_check, PostChangeCallback post_callback,
		std::initializer_list<const char *> many, OnConvert *many_cnvt) :
		OneOfManySettingDesc(save, name, flags, guiproc, startup, patx_name, def, (1 << many.size()) - 1, str, str_help,
			str_val, cat, pre_check, post_callback, many, many_cnvt) {}
	virtual ~ManyOfManySettingDesc() {}

	size_t ParseValue(const char *str) const override;
	void FormatIntValue(char *buf, const char *last, uint32 value) const override;
};

/** String settings. */
struct StringSettingDesc : SettingDesc {
	/**
	 * A check to be performed before the setting gets changed. The passed string may be
	 * changed by the check if that is important, for example to remove unwanted white
	 * space. The return value denotes whether the value, potentially after the changes,
	 * is allowed to be used/set in the configuration.
	 * @param value The prospective new value for the setting.
	 * @return True when the setting is accepted.
	 */
	typedef bool PreChangeCheck(std::string &value);
	/**
	 * A callback to denote that a setting has been changed.
	 * @param The new value for the setting.
	 */
	typedef void PostChangeCallback(const std::string &value);

	StringSettingDesc(const SaveLoad &save, const char *name, SettingFlag flags, OnGuiCtrl *guiproc, bool startup, const char *patx_name, const char *def,
			uint32 max_length, PreChangeCheck pre_check, PostChangeCallback post_callback) :
		SettingDesc(save, name, flags, guiproc, startup, patx_name), def(def == nullptr ? "" : def), max_length(max_length),
			pre_check(pre_check), post_callback(post_callback) {}
	virtual ~StringSettingDesc() {}

	std::string def;                   ///< Default value given when none is present
	uint32 max_length;                 ///< Maximum length of the string, 0 means no maximum length
	PreChangeCheck *pre_check;         ///< Callback to check for the validity of the setting.
	PostChangeCallback *post_callback; ///< Callback when the setting has been changed.

	bool IsStringSetting() const override { return true; }
	void ChangeValue(const void *object, std::string &newval) const;

	void FormatValue(char *buf, const char *last, const void *object) const override;
	void ParseValue(const IniItem *item, void *object) const override;
	bool IsSameValue(const IniItem *item, void *object) const override;
	const std::string &Read(const void *object) const;

private:
	void MakeValueValid(std::string &str) const;
	void Write(const void *object, const std::string &str) const;
};

/** List/array settings. */
struct ListSettingDesc : SettingDesc {
	ListSettingDesc(const SaveLoad &save, const char *name, SettingFlag flags, OnGuiCtrl *guiproc, bool startup, const char *patx_name, const char *def) :
		SettingDesc(save, name, flags, guiproc, startup, patx_name), def(def) {}
	virtual ~ListSettingDesc() {}

	const char *def;        ///< default value given when none is present

	void FormatValue(char *buf, const char *last, const void *object) const override;
	void ParseValue(const IniItem *item, void *object) const override;
	bool IsSameValue(const IniItem *item, void *object) const override;
};

/** Placeholder for settings that have been removed, but might still linger in the savegame. */
struct NullSettingDesc : SettingDesc {
	NullSettingDesc(const SaveLoad &save) :
		SettingDesc(save, "", SF_NOT_IN_CONFIG, nullptr, false, nullptr) {}
	NullSettingDesc(const SaveLoad &save, const char *name, const char *patx_name) :
		SettingDesc(save, name, SF_NOT_IN_CONFIG, nullptr, false, patx_name) {}
	virtual ~NullSettingDesc() {}

	void FormatValue(char *buf, const char *last, const void *object) const override { NOT_REACHED(); }
	void ParseValue(const IniItem *item, void *object) const override { NOT_REACHED(); }
	bool IsSameValue(const IniItem *item, void *object) const override { NOT_REACHED(); }
};

/** Setting cross-reference type. */
struct XrefSettingDesc : SettingDesc {
	XrefSettingDesc(const SaveLoad &save, SettingsXref xref) :
		SettingDesc(SettingDesc::XrefContructorTag(), save, xref) {}
	virtual ~XrefSettingDesc() {}

	void FormatValue(char *buf, const char *last, const void *object) const override { NOT_REACHED(); }
	void ParseValue(const IniItem *item, void *object) const override { NOT_REACHED(); }
	bool IsSameValue(const IniItem *item, void *object) const override { NOT_REACHED(); }
};

typedef std::initializer_list<std::unique_ptr<const SettingDesc>> SettingTable;

const SettingDesc *GetSettingFromName(const char *name);
bool SetSettingValue(const IntSettingDesc *sd, int32 value, bool force_newgame = false);
bool SetSettingValue(const StringSettingDesc *sd, const std::string value, bool force_newgame = false);

#endif /* SETTINGS_INTERNAL_H */
