/**
 * \file mon-lore.c
 * \brief Monster memory code.
 *
 * Copyright (c) 1997-2007 Ben Harrison, James E. Wilson, Robert A. Koeneke
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "effects.h"
#include "init.h"
#include "mon-blows.h"
#include "mon-init.h"
#include "mon-lore.h"
#include "mon-make.h"
#include "mon-predicate.h"
#include "mon-spell.h"
#include "mon-util.h"
#include "obj-gear.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "player-attack.h"
#include "player-calcs.h"
#include "player-timed.h"
#include "project.h"
#include "z-textblock.h"

/**
 * Monster genders
 */
enum monster_sex {
	MON_SEX_NEUTER = 0,
	MON_SEX_MALE,
	MON_SEX_FEMALE,
	MON_SEX_MAX,
};

typedef enum monster_sex monster_sex_t;

/**
 * Determine the color to code a monster spell
 *
 * This function assigns a color to each monster spell, depending on how
 * dangerous the attack is to the player given current state. Spells may be
 * colored green (least dangerous), yellow, orange, or red (most dangerous).
 */
int spell_color(struct player *p, int spell_index)
{
	const struct monster_spell *spell = monster_spell_by_index(spell_index);
	struct effect *eff = spell ? spell->effect : NULL;

	/* No spell */
	if (!spell) return COLOUR_DARK;

	/* Unresistable spells just use the default color */
	if (!spell->lore_attr_resist && !spell->lore_attr_immune) {
		return spell->lore_attr;
	}

	/* Spells with a save */
	if (spell->save_message) {
		/* Mixed results if the save may fail, perfect result if it can't */
		if (p->known_state.skills[SKILL_SAVE] < 100) {
			if (eff->index == EF_TELEPORT_LEVEL) {
				/* Special case - teleport level */
				if (p->known_state.el_info[ELEM_NEXUS].res_level > 0) {
					return spell->lore_attr_resist;
				} else {
					return spell->lore_attr;
				}
			} else if (eff->index == EF_TIMED_INC) {
				/* Simple timed effects */
				if (player_inc_check(p, eff->params[0], true)) {
					return spell->lore_attr;
				} else {
					return spell->lore_attr_resist;
				}
			} else if (spell->lore_attr_immune) {
				/* Multiple timed effects plus damage */
				for (; eff; eff = eff->next) {
					if (eff->index != EF_TIMED_INC) continue;
					if (player_inc_check(p, eff->params[0], true)) {
						return spell->lore_attr;
					}
				}
				return spell->lore_attr_resist;
			} else {
				/* Straight damage */
				return spell->lore_attr;
			}
		} else if (spell->lore_attr_immune) {
			return spell->lore_attr_immune;
		} else {
			return spell->lore_attr_resist;
		}
	}

	/* Bolts, balls and breaths */
	if ((eff->index == EF_BOLT) || (eff->index == EF_BALL) ||
		(eff->index == EF_BREATH)) {
		/* Treat by element */
		switch (eff->params[0]) {
			/* Special case - sound */
			case ELEM_SOUND:
				if (p->known_state.el_info[ELEM_SOUND].res_level > 0) {
					return spell->lore_attr_immune;
				} else if (of_has(p->known_state.flags, OF_PROT_STUN)) {
					return spell->lore_attr_resist;
				} else {
					return spell->lore_attr;
				}
				break;
			/* Special case - nexus */
			case ELEM_NEXUS:
				if (p->known_state.el_info[ELEM_NEXUS].res_level > 0) {
					return spell->lore_attr_immune;
				} else if (p->known_state.skills[SKILL_SAVE] >= 100) {
					return spell->lore_attr_resist;
				} else {
					return spell->lore_attr;
				}
				break;
			/* Elements that stun or confuse */
			case ELEM_FORCE:
			case ELEM_ICE:
			case ELEM_PLASMA:
			case ELEM_WATER:
				if (!of_has(p->known_state.flags, OF_PROT_STUN)) {
					return spell->lore_attr;
				} else if (!of_has(p->known_state.flags, OF_PROT_CONF) &&
						   (eff->params[0] == ELEM_WATER)){
					return spell->lore_attr;
				} else {
					return spell->lore_attr_resist;
				}
				break;
			/* All other elements */
			default:
				if (p->known_state.el_info[eff->params[0]].res_level == 3) {
					return spell->lore_attr_immune;
				} else if (p->known_state.el_info[eff->params[0]].res_level > 0) {
					return spell->lore_attr_resist;
				} else {
					return spell->lore_attr;
				}
		}
	}

	return spell->lore_attr;
}

/**
 * Determine the color to code a monster melee blow effect
 *
 * This function assigns a color to each monster blow effect, depending on how
 * dangerous the attack is to the player given current state. Blows may be
 * colored green (least dangerous), yellow, orange, or red (most dangerous).
 */
int blow_color(struct player *p, int blow_idx)
{
	const struct blow_effect *blow = &blow_effects[blow_idx];

	/* Some blows just use the default color */
	if (!blow->lore_attr_resist && !blow->lore_attr_immune) {
		return blow->lore_attr;
	}

	/* Effects with immunities are straightforward */
	if (blow->lore_attr_immune) {
		int i;

		for (i = ELEM_ACID; i < ELEM_POIS; i++) {
			if (proj_name_to_idx(blow->name) == i) {
				break;
			}
		}

		if (p->known_state.el_info[i].res_level == 3) {
			return blow->lore_attr_immune;
		} else if (p->known_state.el_info[i].res_level > 0) {
			return blow->lore_attr_resist;
		} else {
			return blow->lore_attr;
		}
	}

	/* Now look at what player attributes can protect from the effects */
	if (streq(blow->effect_type, "theft")) {
		if (p->lev + adj_dex_safe[p->known_state.stat_ind[STAT_DEX]] >= 100) {
			return blow->lore_attr_resist;
		} else {
			return blow->lore_attr;
		}
	} else if (streq(blow->effect_type, "drain")) {
		int i;
		bool found = false;
		for (i = 0; i < z_info->pack_size; i++) {
			struct object *obj = player->upkeep->inven[i];
			if (obj && tval_can_have_charges(obj) && obj->pval) {
				found = true;
				break;
			}
		}
		if (found) {
			return blow->lore_attr;
		} else {
			return blow->lore_attr_resist;
		}
	} else if (streq(blow->effect_type, "eat-food")) {
		int i;
		bool found = false;
		for (i = 0; i < z_info->pack_size; i++) {
			struct object *obj = player->upkeep->inven[i];
			if (obj && tval_is_edible(obj)) {
				found = true;
				break;
			}
		}
		if (found) {
			return blow->lore_attr;
		} else {
			return blow->lore_attr_resist;
		}
	} else if (streq(blow->effect_type, "eat-light")) {
		int light_slot = slot_by_name(player, "light");
		struct object *obj = slot_object(player, light_slot);
		if (obj && obj->timeout && !of_has(obj->flags, OF_NO_FUEL)) {
			return blow->lore_attr;
		} else {
			return blow->lore_attr_resist;
		}
	} else if (streq(blow->effect_type, "element")) {
		if (p->known_state.el_info[blow->resist].res_level > 0) {
			return blow->lore_attr_resist;
		} else {
			return blow->lore_attr;
		}
	} else if (streq(blow->effect_type, "flag")) {
		if (of_has(p->known_state.flags, blow->resist)) {
			return blow->lore_attr_resist;
		} else {
			return blow->lore_attr;
		}
	} else if (streq(blow->effect_type, "all_sustains")) {
		if (of_has(p->known_state.flags, OF_SUST_STR) &&
			of_has(p->known_state.flags, OF_SUST_INT) &&
			of_has(p->known_state.flags, OF_SUST_WIS) &&
			of_has(p->known_state.flags, OF_SUST_DEX) &&
			of_has(p->known_state.flags, OF_SUST_CON)) {
			return blow->lore_attr_resist;
		} else {
			return blow->lore_attr;
		}
	}

	return blow->lore_attr;
}

void lore_learn_spell_if_has(struct monster_lore *lore, const struct monster_race *race, int flag)
{
	if (rsf_has(race->spell_flags, flag)) {
		rsf_on(lore->spell_flags, flag);
	}
}

void lore_learn_spell_if_visible(struct monster_lore *lore, const struct monster *mon, int flag)
{
	if (monster_is_visible(mon)) {
		rsf_on(lore->spell_flags, flag);
	}
}

void lore_learn_flag_if_visible(struct monster_lore *lore, const struct monster *mon, int flag)
{
	if (monster_is_visible(mon)) {
		rf_on(lore->flags, flag);
	}
}


/**
 * Update which bits of lore are known
 */
void lore_update(const struct monster_race *race, struct monster_lore *lore)
{
	int i;
	bitflag mask[RF_SIZE];

	if (!race || !lore) return;

	/* Assume some "obvious" flags */
	create_mon_flag_mask(mask, RFT_OBV, RFT_MAX);
	mflag_union(lore->flags, mask);

	/* Blows */
	for (i = 0; i < z_info->mon_blows_max; i++) {
		if (!race->blow) break;
		if (lore->blow_known[i] || lore->blows[i].times_seen ||
			lore->all_known) {
			lore->blow_known[i] = true;
			lore->blows[i].method = race->blow[i].method;
			lore->blows[i].effect = race->blow[i].effect;
			lore->blows[i].dice = race->blow[i].dice;
		}
	}

	/* Killing a monster reveals some properties */
	if ((lore->tkills > 0) || lore->all_known) {
		lore->armour_known = true;
		lore->drop_known = true;
		create_mon_flag_mask(mask, RFT_RACE_A, RFT_RACE_N, RFT_DROP, RFT_MAX);
		mflag_union(lore->flags, mask);
		rf_on(lore->flags, RF_FORCE_DEPTH);
	}

	/* Awareness */
	if ((((int)lore->wake * (int)lore->wake) > race->sleep) ||
	    (lore->ignore == UCHAR_MAX) || lore->all_known ||
	    ((race->sleep == 0) && (lore->tkills >= 10)))
		lore->sleep_known = true;

	/* Spellcasting frequency */
	if ((lore->cast_innate + lore->cast_spell > 100) || lore->all_known)
		lore->spell_freq_known = true;

	/* Flags for probing and cheating */
	if (lore->all_known) {
		rf_setall(lore->flags);
		rsf_copy(lore->spell_flags, race->spell_flags);
	}
}

/**
 * Learn everything about a monster.
 *
 * Sets the all_known variable, all flags and all relevant spell flags.
 */
void cheat_monster_lore(const struct monster_race *race, struct monster_lore *lore)
{
	assert(race);
	assert(lore);

	/* Full knowledge */
	lore->all_known = true;
	lore_update(race, lore);
}

/**
 * Forget everything about a monster.
 */
void wipe_monster_lore(const struct monster_race *race, struct monster_lore *lore)
{
	assert(race);
	assert(lore);

	mem_free(lore->drops);
	mem_free(lore->friends);
	mem_free(lore->friends_base);
	mem_free(lore->mimic_kinds);
	memset(lore, 0, sizeof(*lore));
}

/**
 * Learn about a monster (by "probing" it)
 */
void lore_do_probe(struct monster *mon)
{
	struct monster_lore *lore = get_lore(mon->race);
	
	lore->all_known = true;
	lore_update(mon->race, lore);

	/* Update monster recall window */
	if (player->upkeep->monster_race == mon->race)
		player->upkeep->redraw |= (PR_MONSTER);
}

/**
 * Determine whether the monster is fully known
 */
bool lore_is_fully_known(const struct monster_race *race)
{
	unsigned i;
	struct monster_lore *lore = get_lore(race);

	/* Check if already known */
	if (lore->all_known)
		return true;
		
	if (!lore->armour_known)
		return false;
	/* Only check spells if the monster can cast them */
	if (!lore->spell_freq_known && race->freq_innate + race->freq_spell)
		return false;
	if (!lore->drop_known)
		return false;
	if (!lore->sleep_known)
		return false;
		
	/* Check if blows are known */
	for (i = 0; i < z_info->mon_blows_max; i++){
		/* Only check if the blow exists */
		if (!race->blow[i].method)
			break;
		if (!lore->blow_known[i])
			return false;
		
	}
		
	/* Check all the flags */
	for (i = 0; i < RF_SIZE; i++)
		if (!lore->flags[i])
			return false;
		
		
	/* Check spell flags */
	for (i = 0; i < RSF_SIZE; i++)
		if (lore->spell_flags[i] != race->spell_flags[i])			
			return false;
	
	/* The player knows everything */
	lore->all_known = true;
	lore_update(race, lore);
	return true;
}
	
	
/**
 * Take note that the given monster just dropped some treasure
 *
 * Note that learning the "GOOD"/"GREAT" flags gives information
 * about the treasure (even when the monster is killed for the first
 * time, such as uniques, and the treasure has not been examined yet).
 *
 * This "indirect" method is used to prevent the player from learning
 * exactly how much treasure a monster can drop from observing only
 * a single example of a drop.  This method actually observes how much
 * gold and items are dropped, and remembers that information to be
 * described later by the monster recall code.
 */
void lore_treasure(struct monster *mon, int num_item, int num_gold)
{
	struct monster_lore *lore = get_lore(mon->race);

	assert(num_item >= 0);
	assert(num_gold >= 0);

	/* Note the number of things dropped */
	if (num_item > lore->drop_item)
		lore->drop_item = num_item;
	if (num_gold > lore->drop_gold)
		lore->drop_gold = num_gold;

	/* Learn about drop quality */
	rf_on(lore->flags, RF_DROP_GOOD);
	rf_on(lore->flags, RF_DROP_GREAT);

	/* Update monster recall window */
	if (player->upkeep->monster_race == mon->race)
		player->upkeep->redraw |= (PR_MONSTER);
}

/**
 * Copies into `flags` the flags of the given monster race that are known
 * to the given lore structure (usually the player's knowledge).
 *
 * Known flags will be 1 for present, or 0 for not present. Unknown flags
 * will always be 0.
 */
void monster_flags_known(const struct monster_race *race,
						 const struct monster_lore *lore,
						 bitflag flags[RF_SIZE])
{
	rf_copy(flags, race->flags);
	rf_inter(flags, lore->flags);
}

/**
 * Return a description for the given monster race awareness value.
 *
 * Descriptions are in a table within the function. Returns a sensible string
 * for values not in the table.
 *
 * \param awareness is the inactivity counter of the race (monster_race.sleep).
 */
static const char *lore_describe_awareness(s16b awareness)
{
	/* Value table ordered descending, for priority. Terminator is
	 * {SHRT_MAX, NULL}. */
	static const struct lore_awareness {
		s16b threshold;
		const char *description;
	} lore_awareness_description[] = {
		{200,	"prefers to ignore"},
		{95,	"pays very little attention to"},
		{75,	"pays little attention to"},
		{45,	"tends to overlook"},
		{25,	"takes quite a while to see"},
		{10,	"takes a while to see"},
		{5,		"is fairly observant of"},
		{3,		"is observant of"},
		{1,		"is very observant of"},
		{0,		"is vigilant for"},
		{SHRT_MAX,	NULL},
	};
	const struct lore_awareness *current = lore_awareness_description;

	while (current->threshold != SHRT_MAX && current->description != NULL) {
		if (awareness > current->threshold)
			return current->description;

		current++;
	}

	/* Values zero and less are the most vigilant */
	return "is ever vigilant for";
}

/**
 * Return a description for the given monster race speed value.
 *
 * Descriptions are in a table within the function. Returns a sensible string
 * for values not in the table.
 *
 * \param speed is the speed rating of the race (monster_race.speed).
 */
static const char *lore_describe_speed(byte speed)
{
	/* Value table ordered descending, for priority. Terminator is
	 * {UCHAR_MAX, NULL}. */
	static const struct lore_speed {
		byte threshold;
		const char *description;
	} lore_speed_description[] = {
		{130,	"incredibly quickly"},
		{120,	"very quickly"},
		{110,	"quickly"},
		{109,	"normal speed"}, /* 110 is normal speed */
		{99,	"slowly"},
		{89,	"very slowly"},
		{0,		"incredibly slowly"},
		{UCHAR_MAX,	NULL},
	};
	const struct lore_speed *current = lore_speed_description;

	while (current->threshold != UCHAR_MAX && current->description != NULL) {
		if (speed > current->threshold)
			return current->description;

		current++;
	}

	/* Return a weird description, since the value wasn't found in the table */
	return "erroneously";
}

/**
 * Return a value describing the sex of the provided monster race.
 */
static monster_sex_t lore_monster_sex(const struct monster_race *race)
{
	if (rf_has(race->flags, RF_FEMALE))
		return MON_SEX_FEMALE;
	else if (rf_has(race->flags, RF_MALE))
		return MON_SEX_MALE;

	return MON_SEX_NEUTER;
}

/**
 * Return a pronoun for a monster; used as the subject of a sentence.
 *
 * Descriptions are in a table within the function. Table must match
 * monster_sex_t values.
 *
 * \param sex is the gender value (as provided by `lore_monster_sex()`.
 * \param title_case indicates whether the initial letter should be
 * capitalized; `true` is capitalized, `false` is not.
 */
static const char *lore_pronoun_nominative(monster_sex_t sex, bool title_case)
{
	static const char *lore_pronouns[MON_SEX_MAX][2] = {
		{"it", "It"},
		{"he", "He"},
		{"she", "She"},
	};

	int pronoun_index = MON_SEX_NEUTER, case_index = 0;

	if (sex < MON_SEX_MAX)
		pronoun_index = sex;

	if (title_case)
		case_index = 1;

	return lore_pronouns[pronoun_index][case_index];
}

/**
 * Return a possessive pronoun for a monster.
 *
 * Descriptions are in a table within the function. Table must match
 * monster_sex_t values.
 *
 * \param sex is the gender value (as provided by `lore_monster_sex()`.
 * \param title_case indicates whether the initial letter should be
 * capitalized; `true` is capitalized, `false` is not.
 */
static const char *lore_pronoun_possessive(monster_sex_t sex, bool title_case)
{
	static const char *lore_pronouns[MON_SEX_MAX][2] = {
		{"its", "Its"},
		{"his", "His"},
		{"her", "Her"},
	};

	int pronoun_index = MON_SEX_NEUTER, case_index = 0;

	if (sex < MON_SEX_MAX)
		pronoun_index = sex;

	if (title_case)
		case_index = 1;

	return lore_pronouns[pronoun_index][case_index];
}

/**
 * Append a clause containing a list of descriptions of monster flags from
 * list-mon-race-flags.h to a textblock.
 *
 * The text that joins the list is drawn using the default attributes. The list
 * uses a serial comma ("a, b, c, and d").
 *
 * \param tb is the textblock we are adding to.
 * \param f is the set of flags to be described.
 * \param attr is the attribute each list item will be drawn with.
 * \param start is a string to start the clause.
 * \param conjunction is a string that is added before the last item.
 * \param end is a string that is added after the last item.
 */
static void lore_append_clause(textblock *tb, bitflag *f, byte attr,
							   const char *start, const char *conjunction,
							   const char *end)
{
	int count = rf_count(f);
	bool comma = count > 2;

	if (count) {
		int flag;
		textblock_append(tb, start);
		for (flag = rf_next(f, FLAG_START); flag; flag = rf_next(f, flag + 1)) {
			/* First entry starts immediately */
			if (flag != rf_next(f, FLAG_START)) {
				if (comma) {
					textblock_append(tb, ",");
				}
				/* Last entry */
				if (rf_next(f, flag + 1) == FLAG_END) {
					textblock_append(tb, " ");
					textblock_append(tb, conjunction);
				}
				textblock_append(tb, " ");
			}
			textblock_append_c(tb, attr, describe_race_flag(flag));
		}
		textblock_append(tb, end);
	}
}


/**
 * Append a list of spell descriptions.
 *
 * This is a modified version of `lore_append_clause()` to format spells.
 *
 * \param tb is the textblock we are adding to.
 * \param f is the set of flags to be described.
 * \param know_hp is whether the player knows the monster's AC.
 * \param race is the monster race.
 * \param conjunction is a string that is added before the last item.
 * \param end is a string that is added after the last item.
 */
static void lore_append_spell_clause(textblock *tb, bitflag *f, bool know_hp,
									 const struct monster_race *race,
									 const char *conjunction,
									 const char *end)
{
	int count = rsf_count(f);
	bool comma = count > 2;

	if (count) {
		int spell;
		for (spell = rsf_next(f, FLAG_START); spell;
			 spell = rsf_next(f, spell + 1)) {
			int color = spell_color(player, spell);
			int damage = mon_spell_lore_damage(spell, race, know_hp);

			/* First entry starts immediately */
			if (spell != rsf_next(f, FLAG_START)) {
				if (comma) {
					textblock_append(tb, ",");
				}
				/* Last entry */
				if (rsf_next(f, spell + 1) == FLAG_END) {
					textblock_append(tb, " ");
					textblock_append(tb, conjunction);
				}
				textblock_append(tb, " ");
			}
			textblock_append_c(tb, color,
							   mon_spell_lore_description(spell, race));
			if (damage > 0) {
				textblock_append_c(tb, color, " (%d)", damage);
			}
		}
		textblock_append(tb, end);
	}
}

/**
 * Append the kill history to a texblock for a given monster race.
 *
 * Known race flags are passed in for simplicity/efficiency.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_kills(textblock *tb, const struct monster_race *race,
					   const struct monster_lore *lore,
					   const bitflag known_flags[RF_SIZE])
{
	monster_sex_t msex = MON_SEX_NEUTER;
	bool out = true;

	assert(tb && race && lore);

	/* Extract a gender (if applicable) */
	msex = lore_monster_sex(race);

	/* Treat by whether unique, then by whether they have any player kills */
	if (rf_has(known_flags, RF_UNIQUE)) {
		/* Hack -- Determine if the unique is "dead" */
		bool dead = (race->max_num == 0) ? true : false;

		/* We've been killed... */
		if (lore->deaths) {
			/* Killed ancestors */
			textblock_append(tb, "%s has slain %d of your ancestors",
							 lore_pronoun_nominative(msex, true), lore->deaths);

			/* But we've also killed it */
			if (dead)
				textblock_append(tb, ", but you have taken revenge!  ");

			/* Unavenged (ever) */
			else
				textblock_append(tb, ", who %s unavenged.  ",
								 VERB_AGREEMENT(lore->deaths, "remains",
												"remain"));
		} else if (dead) { /* Dead unique who never hurt us */
			textblock_append(tb, "You have slain this foe.  ");
		} else {
			/* Alive and never killed us */
			out = false;
		}
	} else if (lore->deaths) {
		/* Dead ancestors */
		textblock_append(tb, "%d of your ancestors %s been killed by this creature, ", lore->deaths, VERB_AGREEMENT(lore->deaths, "has", "have"));

		if (lore->pkills) {
			/* Some kills this life */
			textblock_append(tb, "and you have exterminated at least %d of the creatures.  ", lore->pkills);
		} else if (lore->tkills) {
			/* Some kills past lives */
			textblock_append(tb, "and your ancestors have exterminated at least %d of the creatures.  ", lore->tkills);
		} else {
			/* No kills */
			textblock_append_c(tb, COLOUR_RED, "and %s is not ever known to have been defeated.  ", lore_pronoun_nominative(msex, false));
		}
	} else {
		if (lore->pkills) {
			/* Killed some this life */
			textblock_append(tb, "You have killed at least %d of these creatures.  ", lore->pkills);
		} else if (lore->tkills) {
			/* Killed some last life */
			textblock_append(tb, "Your ancestors have killed at least %d of these creatures.  ", lore->tkills);
		} else {
			/* Killed none */
			textblock_append(tb, "No battles to the death are recalled.  ");
		}
	}

	/* Separate */
	if (out)
		textblock_append(tb, "\n");
}

/**
 * Append the monster race description to a textblock.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param append_utf8 indicates if we should append the flavor text as UTF-8
 *        (which is preferred for spoiler files).
 */
void lore_append_flavor(textblock *tb, const struct monster_race *race,
						bool append_utf8)
{
	assert(tb && race);

	if (append_utf8)
		textblock_append_utf8(tb, race->text);
	else
		textblock_append(tb, race->text);

	textblock_append(tb, "\n");
}

/**
 * Append the monster type, location, and movement patterns to a textblock.
 *
 * Known race flags are passed in for simplicity/efficiency.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_movement(textblock *tb, const struct monster_race *race,
						  const struct monster_lore *lore,
						  bitflag known_flags[RF_SIZE])
{
	int f;
	bitflag flags[RF_SIZE];

	assert(tb && race && lore);

	textblock_append(tb, "This");

	/* Get adjectives */
	create_mon_flag_mask(flags, RFT_RACE_A, RFT_MAX);
	rf_inter(flags, race->flags);
	for (f = rf_next(flags, FLAG_START); f; f = rf_next(flags, f + 1)) {
		textblock_append_c(tb, COLOUR_L_BLUE, " %s", describe_race_flag(f));
	}

	/* Get noun */
	create_mon_flag_mask(flags, RFT_RACE_N, RFT_MAX);
	rf_inter(flags, race->flags);
	f = rf_next(flags, FLAG_START);
	if (f) {
		textblock_append_c(tb, COLOUR_L_BLUE, " %s", describe_race_flag(f));
	} else {
		textblock_append_c(tb, COLOUR_L_BLUE, " creature");
	}

	/* Describe location */
	if (race->level == 0) {
		textblock_append(tb, " lives in the town");
	} else {
		byte colour = (race->level > player->max_depth) ? COLOUR_RED :
			COLOUR_L_BLUE;

		if (rf_has(known_flags, RF_FORCE_DEPTH))
			textblock_append(tb, " is found ");
		else
			textblock_append(tb, " is normally found ");

		textblock_append(tb, "at depths of ");
		textblock_append_c(tb, colour, "%d", race->level * 50);
		textblock_append(tb, " feet (level ");
		textblock_append_c(tb, colour, "%d", race->level);
		textblock_append(tb, ")");
	}

	textblock_append(tb, ", and moves");

	/* Random-ness */
	if (flags_test(known_flags, RF_SIZE, RF_RAND_50, RF_RAND_25, FLAG_END)) {
		/* Adverb */
		if (rf_has(known_flags, RF_RAND_50) && rf_has(known_flags, RF_RAND_25))
			textblock_append(tb, " extremely");
		else if (rf_has(known_flags, RF_RAND_50))
			textblock_append(tb, " somewhat");
		else if (rf_has(known_flags, RF_RAND_25))
			textblock_append(tb, " a bit");

		/* Adjective */
		textblock_append(tb, " erratically");

		/* Hack -- Occasional conjunction */
		if (race->speed != 110) textblock_append(tb, ", and");
	}

	/* Speed */
	textblock_append(tb, " ");

	/* "at" is separate from the normal speed description in order to use the
	 * normal text colour */
	if (race->speed == 110)
		textblock_append(tb, "at ");

	textblock_append_c(tb, COLOUR_GREEN, lore_describe_speed(race->speed));

	/* The speed description also describes "attack speed" */
	if (rf_has(known_flags, RF_NEVER_MOVE)) {
		textblock_append(tb, ", but ");
		textblock_append_c(tb, COLOUR_L_GREEN,
						   "does not deign to chase intruders");
	}

	/* End this sentence */
	textblock_append(tb, ".  ");
}

/**
 * Append the monster AC, HP, and hit chance to a textblock.
 *
 * Known race flags are passed in for simplicity/efficiency.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_toughness(textblock *tb, const struct monster_race *race,
						   const struct monster_lore *lore,
						   bitflag known_flags[RF_SIZE])
{
	monster_sex_t msex = MON_SEX_NEUTER;
	long chance = 0, chance2 = 0;
	struct object *weapon = equipped_item_by_slot_name(player, "weapon");

	assert(tb && race && lore);

	/* Extract a gender (if applicable) */
	msex = lore_monster_sex(race);

	/* Describe monster "toughness" */
	if (lore->armour_known) {
		/* Armor */
		textblock_append(tb, "%s has an armor rating of ",
						 lore_pronoun_nominative(msex, true));
		textblock_append_c(tb, COLOUR_L_BLUE, "%d", race->ac);

		/* Hitpoints */
		textblock_append(tb, ", and a");

		if (!rf_has(known_flags, RF_UNIQUE))
			textblock_append(tb, "n average");

		textblock_append(tb, " life rating of ");
		textblock_append_c(tb, COLOUR_L_BLUE, "%d", race->avg_hp);
		textblock_append(tb, ".  ");

		/* Player's base chance to hit */
		chance = py_attack_hit_chance(player, weapon);

		/* The following calculations are based on test_hit();
		 * make sure to keep it in sync */
		if (chance < 9) {
			chance = 9;
		}
		chance2 = 12 + (100 - 12 - 5) * (chance - (race->ac * 2 / 3)) / chance;
		if (chance2 < 12) {
			chance2 = 12;
		}

		textblock_append(tb, "You have a");
		if ((chance2 == 8) || ((chance2 / 10) == 8))
			textblock_append(tb, "n");
		textblock_append_c(tb, COLOUR_L_BLUE, " %d", chance2);
		textblock_append(tb, " percent chance to hit such a creature in melee (if you can see it).  ");
	}
}

/**
 * Append the experience value description to a textblock.
 *
 * Known race flags are passed in for simplicity/efficiency.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_exp(textblock *tb, const struct monster_race *race,
					 const struct monster_lore *lore,
					 bitflag known_flags[RF_SIZE])
{
	const char *ordinal, *article;
	char buf[20] = "";
	long exp_integer, exp_fraction;
	s16b level;

	assert(tb && race && lore);

	/* Introduction */
	if (rf_has(known_flags, RF_UNIQUE))
		textblock_append(tb, "Killing");
	else
		textblock_append(tb, "A kill of");

	textblock_append(tb, " this creature");

	/* calculate the integer exp part */
	exp_integer = (long)race->mexp * race->level / player->lev;

	/* calculate the fractional exp part scaled by 100, must use long
	 * arithmetic to avoid overflow */
	exp_fraction = ((((long)race->mexp * race->level % player->lev) *
					 (long)1000 / player->lev + 5) / 10);

	/* Calculate textual representation */
	strnfmt(buf, sizeof(buf), "%d", exp_integer);
	if (exp_fraction)
		my_strcat(buf, format(".%02d", exp_fraction), sizeof(buf));

	/* Mention the experience */
	textblock_append(tb, " is worth ");
	textblock_append_c(tb, COLOUR_BLUE, format("%s point%s", buf, PLURAL((exp_integer == 1) && (exp_fraction == 0))));

	/* Take account of annoying English */
	ordinal = "th";
	level = player->lev % 10;
	if ((player->lev / 10) == 1) /* nothing */;
	else if (level == 1) ordinal = "st";
	else if (level == 2) ordinal = "nd";
	else if (level == 3) ordinal = "rd";

	/* Take account of "leading vowels" in numbers */
	article = "a";
	level = player->lev;
	if ((level == 8) || (level == 11) || (level == 18)) article = "an";

	/* Mention the dependance on the player's level */
	textblock_append(tb, " for %s %u%s level character.  ", article,
					 level, ordinal);
}

/**
 * Append the monster drop description to a textblock.
 *
 * Known race flags are passed in for simplicity/efficiency.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_drop(textblock *tb, const struct monster_race *race,
					  const struct monster_lore *lore,
					  bitflag known_flags[RF_SIZE])
{
	int n = 0;
	monster_sex_t msex = MON_SEX_NEUTER;

	assert(tb && race && lore);
	if (!lore->drop_known) return;

	/* Extract a gender (if applicable) */
	msex = lore_monster_sex(race);

	/* Count maximum drop */
	n = mon_create_drop_count(race, true);

	/* Drops gold and/or items */
	if (n > 0) {
		bool only_item = rf_has(known_flags, RF_ONLY_ITEM);
		bool only_gold = rf_has(known_flags, RF_ONLY_GOLD);

		textblock_append(tb, "%s may carry",
						 lore_pronoun_nominative(msex, true));

		/* Count drops */
		if (n == 1)
			textblock_append_c(tb, COLOUR_BLUE, " a single ");
		else if (n == 2)
			textblock_append_c(tb, COLOUR_BLUE, " one or two ");
		else {
			textblock_append(tb, " up to ");
			textblock_append_c(tb, COLOUR_BLUE, format("%d ", n));
		}

		/* Quality */
		if (rf_has(known_flags, RF_DROP_GREAT))
			textblock_append_c(tb, COLOUR_BLUE, "exceptional ");
		else if (rf_has(known_flags, RF_DROP_GOOD))
			textblock_append_c(tb, COLOUR_BLUE, "good ");

		/* Objects or treasures */
		if (only_item && only_gold)
			textblock_append_c(tb, COLOUR_BLUE, "error%s", PLURAL(n));
		else if (only_item && !only_gold)
			textblock_append_c(tb, COLOUR_BLUE, "object%s", PLURAL(n));
		else if (!only_item && only_gold)
			textblock_append_c(tb, COLOUR_BLUE, "treasure%s", PLURAL(n));
		else if (!only_item && !only_gold)
			textblock_append_c(tb, COLOUR_BLUE, "object%s or treasure%s",
							   PLURAL(n), PLURAL(n));

		textblock_append(tb, ".  ");
	}
}

/**
 * Append the monster abilities (resists, weaknesses, other traits) to a
 * textblock.
 *
 * Known race flags are passed in for simplicity/efficiency. Note the macros
 * that are used to simplify the code.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_abilities(textblock *tb, const struct monster_race *race,
						   const struct monster_lore *lore,
						   bitflag known_flags[RF_SIZE])
{
	int flag;
	char start[40];
	const char *initial_pronoun;
	bool prev = false;
	bitflag current_flags[RF_SIZE], test_flags[RF_SIZE];
	monster_sex_t msex = MON_SEX_NEUTER;

	assert(tb && race && lore);

	/* Extract a gender (if applicable) and get a pronoun for the start of
	 * sentences */
	msex = lore_monster_sex(race);
	initial_pronoun = lore_pronoun_nominative(msex, true);

	/* Describe environment-shaping abilities. */
	create_mon_flag_mask(current_flags, RFT_ALTER, RFT_MAX);
	rf_inter(current_flags, known_flags);
	my_strcpy(start, format("%s can ", initial_pronoun), sizeof(start));
	lore_append_clause(tb, current_flags, COLOUR_WHITE, start, "and", ". ");

	/* Describe detection traits */
	create_mon_flag_mask(current_flags, RFT_DET, RFT_MAX);
	rf_inter(current_flags, known_flags);
	my_strcpy(start, format("%s is ", initial_pronoun), sizeof(start));
	lore_append_clause(tb, current_flags, COLOUR_WHITE, start, "and", ". ");

	/* Describe special things */
	if (rf_has(known_flags, RF_UNAWARE))
		textblock_append(tb, "%s disguises itself as something else.  ",
						 initial_pronoun);
	if (rf_has(known_flags, RF_MULTIPLY))
		textblock_append_c(tb, COLOUR_ORANGE, "%s breeds explosively.  ",
						   initial_pronoun);
	if (rf_has(known_flags, RF_REGENERATE))
		textblock_append(tb, "%s regenerates quickly.  ", initial_pronoun);
	if (rf_has(known_flags, RF_HAS_LIGHT))
		textblock_append(tb, "%s illuminates %s surroundings.  ",
						 initial_pronoun, lore_pronoun_possessive(msex, false));

	/* Collect susceptibilities */
	create_mon_flag_mask(current_flags, RFT_VULN, RFT_VULN_I, RFT_MAX);
	rf_inter(current_flags, known_flags);
	my_strcpy(start, format("%s is hurt by ", initial_pronoun), sizeof(start));
	lore_append_clause(tb, current_flags, COLOUR_VIOLET, start, "and", "");
	if (!rf_is_empty(current_flags)) {
		prev = true;
	}

	/* Collect immunities and resistances */
	create_mon_flag_mask(current_flags, RFT_RES, RFT_MAX);
	rf_inter(current_flags, known_flags);

	/* Note lack of vulnerability as a resistance */
	create_mon_flag_mask(test_flags, RFT_VULN, RFT_MAX);
	for (flag = rf_next(test_flags, FLAG_START); flag;
		 flag = rf_next(test_flags, flag + 1)) {
		if (rf_has(lore->flags, flag) && !rf_has(known_flags, flag)) {
			rf_on(current_flags, flag);
		}
	}
	if (prev)
		my_strcpy(start, ", but resists ", sizeof(start));
	else
		my_strcpy(start, format("%s resists ", initial_pronoun), sizeof(start));
	lore_append_clause(tb, current_flags, COLOUR_L_UMBER, start, "and", "");
	if (!rf_is_empty(current_flags)) {
		prev = true;
	}

	/* Collect known but average susceptibilities */
	rf_wipe(current_flags);
	create_mon_flag_mask(test_flags, RFT_RES, RFT_MAX);
	for (flag = rf_next(test_flags, FLAG_START); flag;
		 flag = rf_next(test_flags, flag + 1)) {
		if (rf_has(lore->flags, flag) && !rf_has(known_flags, flag)) {
			rf_on(current_flags, flag);
		}
	}

	/* Vulnerabilities need to be specifically removed */
	create_mon_flag_mask(test_flags, RFT_VULN_I, RFT_MAX);
	rf_inter(test_flags, known_flags);
	for (flag = rf_next(test_flags, FLAG_START); flag;
		 flag = rf_next(test_flags, flag + 1)) {
		int susc_flag;
		for (susc_flag = rf_next(current_flags, FLAG_START); susc_flag;
			 susc_flag = rf_next(current_flags, susc_flag + 1)) {
			if (streq(describe_race_flag(flag), describe_race_flag(susc_flag)))
				rf_off(current_flags, susc_flag);
		}
	}
	if (prev)
		my_strcpy(start, ", and does not resist ", sizeof(start));
	else
		my_strcpy(start, format("%s does not resist ", initial_pronoun),
				  sizeof(start));
	lore_append_clause(tb, current_flags, COLOUR_L_UMBER, start, "or", "");
	if (!rf_is_empty(current_flags)) {
		prev = true;
	}

	/* Collect non-effects */
	create_mon_flag_mask(current_flags, RFT_PROT, RFT_MAX);
	rf_inter(current_flags, known_flags);
	if (prev)
		my_strcpy(start, ", and cannot be ", sizeof(start));
	else
		my_strcpy(start, format("%s cannot be ", initial_pronoun),
				  sizeof(start));
	lore_append_clause(tb, current_flags, COLOUR_L_UMBER, start, "or", "");

	if (prev)
		textblock_append(tb, ".  ");
}

/**
 * Append how the monster reacts to intruders and at what distance it does so.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_awareness(textblock *tb, const struct monster_race *race,
						   const struct monster_lore *lore,
						   bitflag known_flags[RF_SIZE])
{
	monster_sex_t msex = MON_SEX_NEUTER;

	assert(tb && race && lore);

	/* Extract a gender (if applicable) */
	msex = lore_monster_sex(race);

	/* Do we know how aware it is? */
	if (lore->sleep_known)
	{
		const char *aware = lore_describe_awareness(race->sleep);
		textblock_append(tb, "%s %s intruders, which %s may notice from ",
						 lore_pronoun_nominative(msex, true), aware,
						 lore_pronoun_nominative(msex, false));
		textblock_append_c(tb, COLOUR_L_BLUE, "%d", 10 * race->hearing);
		textblock_append(tb, " feet.  ");
	}
}

/**
 * Append information about what other races the monster appears with and if
 * they work together.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_friends(textblock *tb, const struct monster_race *race,
						 const struct monster_lore *lore,
						 bitflag known_flags[RF_SIZE])
{
	monster_sex_t msex = MON_SEX_NEUTER;

	assert(tb && race && lore);

	/* Extract a gender (if applicable) */
	msex = lore_monster_sex(race);

	/* Describe friends */
	if (race->friends || race->friends_base) {
		textblock_append(tb, "%s may appear with other monsters",
						 lore_pronoun_nominative(msex, true));
		if (rf_has(known_flags, RF_GROUP_AI))
			textblock_append(tb, " and hunts in packs");
		textblock_append(tb, ".  ");
	}
}

/**
 * Append the monster's attack spells to a textblock.
 *
 * Known race flags are passed in for simplicity/efficiency. Note the macros
 * that are used to simplify the code.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 */
void lore_append_spells(textblock *tb, const struct monster_race *race,
						const struct monster_lore *lore,
						bitflag known_flags[RF_SIZE])
{
	int average_frequency;
	monster_sex_t msex = MON_SEX_NEUTER;
	bool breath = false;
	bool magic = false;
	const char *initial_pronoun;
	bool know_hp;
	bitflag current_flags[RSF_SIZE], test_flags[RSF_SIZE];

	assert(tb && race && lore);

	know_hp = lore->armour_known;

	/* Extract a gender (if applicable) and get a pronoun for the start of
	 * sentences */
	msex = lore_monster_sex(race);
	initial_pronoun = lore_pronoun_nominative(msex, true);

	/* Collect innate (non-breath) attacks */
	create_mon_spell_mask(current_flags, RST_INNATE, RST_NONE);
	rsf_inter(current_flags, lore->spell_flags);
	create_mon_spell_mask(test_flags, RST_BREATH, RST_NONE);
	rsf_diff(current_flags, test_flags);
	if (!rsf_is_empty(current_flags)) {
		textblock_append(tb, "%s may ", initial_pronoun);
		lore_append_spell_clause(tb, current_flags, know_hp, race, "or", ".  ");
	}

	/* Collect breaths */
	create_mon_spell_mask(current_flags, RST_BREATH, RST_NONE);
	rsf_inter(current_flags, lore->spell_flags);
	if (!rsf_is_empty(current_flags)) {
		textblock_append(tb, "%s may ", initial_pronoun);
		textblock_append_c(tb, COLOUR_L_RED, "breathe ");
		lore_append_spell_clause(tb, current_flags, know_hp, race, "or", "");
		breath = true;
	}


	/* Collect spell information */
	rsf_copy(current_flags, lore->spell_flags);
	create_mon_spell_mask(test_flags, RST_BREATH, RST_INNATE, RST_NONE);
	rsf_diff(current_flags, test_flags);
	if (!rsf_is_empty(current_flags)) {
		magic = true;

		/* Intro */
		if (breath)
			textblock_append(tb, ", and may ");
		else
			textblock_append(tb, "%s may ", initial_pronoun);

		/* Verb Phrase */
		textblock_append_c(tb, COLOUR_L_RED, "cast spells");

		/* Adverb */
		if (rf_has(known_flags, RF_SMART))
			textblock_append(tb, " intelligently");

		/* List */
		textblock_append(tb, " which ");
		lore_append_spell_clause(tb, current_flags, know_hp, race, "or", "");
	}

	/* End the sentence about innate/other spells */
	if (breath || magic) {
		/* Calculate total casting and average frequency */
		average_frequency = (race->freq_innate + race->freq_spell) / 2;

		if (lore->spell_freq_known) {
			/* Describe the spell frequency */
			textblock_append(tb, "; ");
			textblock_append_c(tb, COLOUR_L_GREEN, "1");
			textblock_append(tb, " time in ");
			textblock_append_c(tb, COLOUR_L_GREEN, "%d", 100 / average_frequency);
		} else if (lore->cast_innate || lore->cast_spell) {
			/* Guess at the frequency */
			average_frequency = ((average_frequency + 9) / 10) * 10;
			textblock_append(tb, "; about ");
			textblock_append_c(tb, COLOUR_L_GREEN, "1");
			textblock_append(tb, " time in ");
			textblock_append_c(tb, COLOUR_L_GREEN, "%d", 100 / average_frequency);
		}

		textblock_append(tb, ".  ");
	}
}

/**
 * Append the monster's melee attacks to a textblock.
 *
 * Known race flags are passed in for simplicity/efficiency.
 *
 * \param tb is the textblock we are adding to.
 * \param race is the monster race we are describing.
 * \param lore is the known information about the monster race.
 * \param known_flags is the preprocessed bitfield of race flags known to the
 *        player.
 * \param melee_colors is a list of colors that is associated with each
 *        blow effect.
 */
void lore_append_attack(textblock *tb, const struct monster_race *race,
						const struct monster_lore *lore,
						bitflag known_flags[RF_SIZE])
{
	int i, total_attacks, described_count;
	monster_sex_t msex = MON_SEX_NEUTER;

	assert(tb && race && lore);

	/* Extract a gender (if applicable) */
	msex = lore_monster_sex(race);

	/* Notice lack of attacks */
	if (rf_has(known_flags, RF_NEVER_BLOW)) {
		textblock_append(tb, "%s has no physical attacks.  ",
						 lore_pronoun_nominative(msex, true));
		return;
	}

	/* Count the number of known attacks */
	for (total_attacks = 0, i = 0; i < z_info->mon_blows_max; i++) {
		/* Skip non-attacks */
		if (!race->blow[i].method) continue;

		/* Count known attacks */
		if (lore->blow_known[i])
			total_attacks++;
	}

	/* Describe the lack of knowledge */
	if (total_attacks == 0) {
		textblock_append(tb, "Nothing is known about %s attack.  ",
						 lore_pronoun_possessive(msex, false));
		return;
	}

	described_count = 0;

	/* Describe each melee attack */
	for (i = 0; i < z_info->mon_blows_max; i++) {
		random_value dice;
		const char *effect_str = NULL;

		/* Skip unknown and undefined attacks */
		if (!race->blow[i].method || !lore->blow_known[i]) continue;

		/* Extract the attack info */
		dice = race->blow[i].dice;
		effect_str = race->blow[i].effect->desc;

		/* Introduce the attack description */
		if (described_count == 0)
			textblock_append(tb, "%s can ",
							 lore_pronoun_nominative(msex, true));
		else if (described_count < total_attacks - 1)
			textblock_append(tb, ", ");
		else
			textblock_append(tb, ", and ");

		/* Describe the method */
		textblock_append(tb, race->blow[i].method->desc);

		/* Describe the effect (if any) */
		if (effect_str && strlen(effect_str) > 0) {
			int index = blow_index(race->blow[i].effect->name);
			/* Describe the attack type */
			textblock_append(tb, " to ");
			textblock_append_c(tb, blow_color(player, index), effect_str);

			/* Describe damage (if known) */
			if (dice.base || dice.dice || dice.sides || dice.m_bonus) {
				textblock_append(tb, " with damage ");

				if (dice.base)
					textblock_append_c(tb, COLOUR_L_GREEN, "%d", dice.base);

				if (dice.dice && dice.sides)
					textblock_append_c(tb, COLOUR_L_GREEN, "%dd%d", dice.dice, dice.sides);

				if (dice.m_bonus)
					textblock_append_c(tb, COLOUR_L_GREEN, "M%d", dice.m_bonus);
			}

		}

		described_count++;
	}

	textblock_append(tb, ".  ");
}

/**
 * Get the lore record for this monster race.
 */
struct monster_lore *get_lore(const struct monster_race *race)
{
	assert(race);
	return &l_list[race->ridx];
}


/**
 * Write the monster lore
 */
void write_lore_entries(ang_file *fff)
{
	int i, n;

	for (i = 0; i < z_info->r_max; i++) {
		/* Current entry */
		struct monster_race *race = &r_info[i];
		struct monster_lore *lore = &l_list[i];

		/* Ignore non-existent or unseen monsters */
		if (!race->name) continue;
		if (!lore->sights && !lore->all_known) continue;

		/* Output 'name' */
		file_putf(fff, "name:%s\n", race->name);

		/* Output base if we're remembering everything */
		if (lore->all_known)
			file_putf(fff, "base:%s\n", race->base->name);

		/* Output counts */
		file_putf(fff, "counts:%d:%d:%d:%d:%d:%d:%d\n", lore->sights,
				  lore->deaths, lore->tkills, lore->wake, lore->ignore,
				  lore->cast_innate, lore->cast_spell);

		/* Output blow (up to max blows) */
		for (n = 0; n < z_info->mon_blows_max; n++) {
			/* End of blows */
			if (!lore->blow_known[n] && !lore->all_known) continue;
			if (!lore->blows[n].method) continue;

			/* Output blow method */
			file_putf(fff, "blow:%s", lore->blows[n].method->name);

			/* Output blow effect (may be none) */
			file_putf(fff, ":%s", lore->blows[n].effect->name);

			/* Output blow damage (may be 0) */
			file_putf(fff, ":%d+%dd%dM%d", lore->blows[n].dice.base,
					lore->blows[n].dice.dice,
					lore->blows[n].dice.sides,
					lore->blows[n].dice.m_bonus);

			/* Output number of times that blow has been seen */
			file_putf(fff, ":%d", lore->blows[n].times_seen);

			/* Output blow index */
			file_putf(fff, ":%d", n);

			/* End line */
			file_putf(fff, "\n");
		}

		/* Output flags */
		write_flags(fff, "flags:", lore->flags, RF_SIZE, r_info_flags);

		/* Output spell flags (multiple lines) */
		rsf_inter(lore->spell_flags, race->spell_flags);
		write_flags(fff, "spells:", lore->spell_flags, RSF_SIZE,
					r_info_spell_flags);

		/* Output 'drop' */
		if (lore->drops) {
			struct monster_drop *drop = lore->drops;
			struct object_kind *kind = drop->kind;
			char name[120] = "";

			while (drop) {
				object_short_name(name, sizeof name, kind->name);
				file_putf(fff, "drop:%s:%s:%d:%d:%d\n",
						  tval_find_name(kind->tval), name,
						  drop->percent_chance, drop->min, drop->max);
				drop = drop->next;
			}
		}

		/* Output 'friends' */
		if (lore->friends) {
			struct monster_friends *f = lore->friends;

			while (f) {
				file_putf(fff, "friends:%d:%dd%d:%s\n", f->percent_chance,
						  f->number_dice, f->number_side, f->race->name);
				f = f->next;
			}
		}

		/* Output 'friends-base' */
		if (lore->friends_base) {
			struct monster_friends_base *b = lore->friends_base;

			while (b) {
				file_putf(fff, "friends-base:%d:%dd%d:%s\n", b->percent_chance,
						  b->number_dice, b->number_side, b->base->name);
				b = b->next;
			}
		}

		/* Output 'mimic' */
		if (lore->mimic_kinds) {
			struct monster_mimic *m = lore->mimic_kinds;
			struct object_kind *kind = m->kind;
			char name[120] = "";

			while (m) {
				object_short_name(name, sizeof name, kind->name);
				file_putf(fff, "mimic:%s:%s\n",
						  tval_find_name(kind->tval), name);
				m = m->next;
			}
		}

		file_putf(fff, "\n");
	}
}


/**
 * Save the lore to a file in the user directory.
 *
 * \param name is the filename
 *
 * \returns true on success, false otherwise.
 */
bool lore_save(const char *name)
{
	char path[1024];

	/* Write to the user directory */
	path_build(path, sizeof(path), ANGBAND_DIR_USER, name);

	if (text_lines_to_file(path, write_lore_entries)) {
		msg("Failed to create file %s.new", path);
		return false;
	}

	return true;
}
