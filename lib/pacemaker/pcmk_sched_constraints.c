/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <sys/types.h>
#include <stdbool.h>
#include <regex.h>
#include <glib.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>
#include <crm/common/iso8601.h>
#include <crm/pengine/status.h>
#include <crm/pengine/internal.h>
#include <crm/pengine/rules.h>
#include <pacemaker-internal.h>

enum pe_order_kind {
    pe_order_kind_optional,
    pe_order_kind_mandatory,
    pe_order_kind_serialize,
};

enum ordering_symmetry {
    ordering_asymmetric,        // the only relation in an asymmetric ordering
    ordering_symmetric,         // the normal relation in a symmetric ordering
    ordering_symmetric_inverse, // the inverse relation in a symmetric ordering
};

#define EXPAND_CONSTRAINT_IDREF(__set, __rsc, __name) do {				\
	__rsc = pe_find_constraint_resource(data_set->resources, __name);		\
	if(__rsc == NULL) {						\
	    pcmk__config_err("%s: No resource found for %s", __set, __name);    \
	    return FALSE;						\
	}								\
    } while(0)

static pe__location_t *generate_location_rule(pe_resource_t *rsc,
                                              xmlNode *rule_xml,
                                              const char *discovery,
                                              crm_time_t *next_change,
                                              pe_working_set_t *data_set,
                                              pe_re_match_data_t *match_data);
static void unpack_location(xmlNode *xml_obj, pe_working_set_t *data_set);
static void unpack_rsc_colocation(xmlNode *xml_obj, pe_working_set_t *data_set);
static void unpack_rsc_order(xmlNode *xml_obj, pe_working_set_t *data_set);
static void unpack_rsc_ticket(xmlNode *xml_obj, pe_working_set_t *data_set);

static bool
evaluate_lifetime(xmlNode *lifetime, pe_working_set_t *data_set)
{
    bool result = FALSE;
    crm_time_t *next_change = crm_time_new_undefined();

    result = pe_evaluate_rules(lifetime, NULL, data_set->now, next_change);
    if (crm_time_is_defined(next_change)) {
        time_t recheck = (time_t) crm_time_get_seconds_since_epoch(next_change);

        pe__update_recheck_time(recheck, data_set);
    }
    crm_time_free(next_change);
    return result;
}

gboolean
unpack_constraints(xmlNode * xml_constraints, pe_working_set_t * data_set)
{
    xmlNode *xml_obj = NULL;
    xmlNode *lifetime = NULL;

    for (xml_obj = pcmk__xe_first_child(xml_constraints); xml_obj != NULL;
         xml_obj = pcmk__xe_next(xml_obj)) {
        const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
        const char *tag = crm_element_name(xml_obj);

        if (id == NULL) {
            pcmk__config_err("Ignoring <%s> constraint without "
                             XML_ATTR_ID, tag);
            continue;
        }

        crm_trace("Unpacking %s constraint '%s'", tag, id);

        lifetime = first_named_child(xml_obj, "lifetime");
        if (lifetime) {
            pcmk__config_warn("Support for 'lifetime' attribute (in %s) is "
                              "deprecated (the rules it contains should "
                              "instead be direct descendents of the "
                              "constraint object)", id);
        }

        if (lifetime && !evaluate_lifetime(lifetime, data_set)) {
            crm_info("Constraint %s %s is not active", tag, id);

        } else if (pcmk__str_eq(XML_CONS_TAG_RSC_ORDER, tag, pcmk__str_casei)) {
            unpack_rsc_order(xml_obj, data_set);

        } else if (pcmk__str_eq(XML_CONS_TAG_RSC_DEPEND, tag, pcmk__str_casei)) {
            unpack_rsc_colocation(xml_obj, data_set);

        } else if (pcmk__str_eq(XML_CONS_TAG_RSC_LOCATION, tag, pcmk__str_casei)) {
            unpack_location(xml_obj, data_set);

        } else if (pcmk__str_eq(XML_CONS_TAG_RSC_TICKET, tag, pcmk__str_casei)) {
            unpack_rsc_ticket(xml_obj, data_set);

        } else {
            pe_err("Unsupported constraint type: %s", tag);
        }
    }

    return TRUE;
}

static const char *
invert_action(const char *action)
{
    if (pcmk__str_eq(action, RSC_START, pcmk__str_casei)) {
        return RSC_STOP;

    } else if (pcmk__str_eq(action, RSC_STOP, pcmk__str_casei)) {
        return RSC_START;

    } else if (pcmk__str_eq(action, RSC_PROMOTE, pcmk__str_casei)) {
        return RSC_DEMOTE;

    } else if (pcmk__str_eq(action, RSC_DEMOTE, pcmk__str_casei)) {
        return RSC_PROMOTE;

    } else if (pcmk__str_eq(action, RSC_PROMOTED, pcmk__str_casei)) {
        return RSC_DEMOTED;

    } else if (pcmk__str_eq(action, RSC_DEMOTED, pcmk__str_casei)) {
        return RSC_PROMOTED;

    } else if (pcmk__str_eq(action, RSC_STARTED, pcmk__str_casei)) {
        return RSC_STOPPED;

    } else if (pcmk__str_eq(action, RSC_STOPPED, pcmk__str_casei)) {
        return RSC_STARTED;
    }
    crm_warn("Unknown action '%s' specified in order constraint", action);
    return NULL;
}

static enum pe_order_kind
get_ordering_type(xmlNode * xml_obj)
{
    enum pe_order_kind kind_e = pe_order_kind_mandatory;
    const char *kind = crm_element_value(xml_obj, XML_ORDER_ATTR_KIND);

    if (kind == NULL) {
        const char *score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);

        kind_e = pe_order_kind_mandatory;

        if (score) {
            // @COMPAT deprecated informally since 1.0.7, formally since 2.0.1
            int score_i = char2score(score);

            if (score_i == 0) {
                kind_e = pe_order_kind_optional;
            }
            pe_warn_once(pe_wo_order_score,
                         "Support for 'score' in rsc_order is deprecated "
                         "and will be removed in a future release (use 'kind' instead)");
        }

    } else if (pcmk__str_eq(kind, "Mandatory", pcmk__str_casei)) {
        kind_e = pe_order_kind_mandatory;

    } else if (pcmk__str_eq(kind, "Optional", pcmk__str_casei)) {
        kind_e = pe_order_kind_optional;

    } else if (pcmk__str_eq(kind, "Serialize", pcmk__str_casei)) {
        kind_e = pe_order_kind_serialize;

    } else {
        pcmk__config_err("Resetting '" XML_ORDER_ATTR_KIND "' for constraint "
                         "'%s' to Mandatory because '%s' is not valid",
                         crm_str(ID(xml_obj)), kind);
    }
    return kind_e;
}

static pe_resource_t *
pe_find_constraint_resource(GList *rsc_list, const char *id)
{
    GList *rIter = NULL;

    for (rIter = rsc_list; id && rIter; rIter = rIter->next) {
        pe_resource_t *parent = rIter->data;
        pe_resource_t *match = parent->fns->find_rsc(parent, id, NULL,
                                                     pe_find_renamed);

        if (match != NULL) {
            if(!pcmk__str_eq(match->id, id, pcmk__str_casei)) {
                /* We found an instance of a clone instead */
                match = uber_parent(match);
                crm_debug("Found %s for %s", match->id, id);
            }
            return match;
        }
    }
    crm_trace("No match for %s", id);
    return NULL;
}

static gboolean
pe_find_constraint_tag(pe_working_set_t * data_set, const char * id, pe_tag_t ** tag)
{
    gboolean rc = FALSE;

    *tag = NULL;
    rc = g_hash_table_lookup_extended(data_set->template_rsc_sets, id,
                                       NULL, (gpointer*) tag);

    if (rc == FALSE) {
        rc = g_hash_table_lookup_extended(data_set->tags, id,
                                          NULL, (gpointer*) tag);

        if (rc == FALSE) {
            crm_warn("No template or tag named '%s'", id);
            return FALSE;

        } else if (*tag == NULL) {
            crm_warn("No resource is tagged with '%s'", id);
            return FALSE;
        }

    } else if (*tag == NULL) {
        crm_warn("No resource is derived from template '%s'", id);
        return FALSE;
    }

    return rc;
}

static gboolean
valid_resource_or_tag(pe_working_set_t * data_set, const char * id,
                      pe_resource_t ** rsc, pe_tag_t ** tag)
{
    gboolean rc = FALSE;

    if (rsc) {
        *rsc = NULL;
        *rsc = pe_find_constraint_resource(data_set->resources, id);
        if (*rsc) {
            return TRUE;
        }
    }

    if (tag) {
        *tag = NULL;
        rc = pe_find_constraint_tag(data_set, id, tag);
    }

    return rc;
}

/*!
 * \internal
 * \brief Get ordering symmetry from XML
 *
 * \param[in] xml_obj               Ordering XML
 * \param[in] parent_kind           Default ordering kind
 * \param[in] parent_symmetrical_s  Parent element's symmetrical setting, if any
 *
 * \retval ordering_symmetric   Ordering is symmetric
 * \retval ordering_asymmetric  Ordering is asymmetric
 */
static enum ordering_symmetry
get_ordering_symmetry(xmlNode *xml_obj, enum pe_order_kind parent_kind,
                      const char *parent_symmetrical_s)
{
    const char *symmetrical_s = NULL;
    enum pe_order_kind kind = parent_kind; // Default to parent's kind

    // Check ordering XML for explicit kind
    if ((crm_element_value(xml_obj, XML_ORDER_ATTR_KIND) != NULL)
        || (crm_element_value(xml_obj, XML_RULE_ATTR_SCORE) != NULL)) {
        kind = get_ordering_type(xml_obj);
    }

    // Check ordering XML (and parent) for explicit symmetrical setting
    symmetrical_s = crm_element_value(xml_obj, XML_CONS_ATTR_SYMMETRICAL);
    if (symmetrical_s == NULL) {
        symmetrical_s = parent_symmetrical_s;
    }
    if (symmetrical_s != NULL) {
        if (crm_is_true(symmetrical_s)) {
            if (kind == pe_order_kind_serialize) {
                pcmk__config_warn("Ignoring " XML_CONS_ATTR_SYMMETRICAL
                                  " for '%s' because not valid with "
                                  XML_ORDER_ATTR_KIND " of 'Serialize'",
                                  ID(xml_obj));
            } else {
                return ordering_symmetric;
            }
        }
        return ordering_asymmetric;
    }

    // Use default symmetry
    if (kind == pe_order_kind_serialize) {
        return ordering_asymmetric;
    }
    return ordering_symmetric;
}

/*!
 * \internal
 * \brief Get ordering flags appropriate to ordering kind
 *
 * \param[in] kind      Ordering kind
 * \param[in] first     Action name for 'first' action
 * \param[in] symmetry  This ordering's symmetry role
 *
 * \return Minimal ordering flags appropriate to \p kind
 */
static enum pe_ordering
ordering_flags_for_kind(enum pe_order_kind kind, const char *first,
                        enum ordering_symmetry symmetry)
{
    enum pe_ordering flags = pe_order_none; // so we trace-log all flags set

    pe__set_order_flags(flags, pe_order_optional);

    switch (kind) {
        case pe_order_kind_optional:
            break;

        case pe_order_kind_serialize:
            pe__set_order_flags(flags, pe_order_serialize_only);
            break;

        case pe_order_kind_mandatory:
            switch (symmetry) {
                case ordering_asymmetric:
                    pe__set_order_flags(flags, pe_order_asymmetrical);
                    break;

                case ordering_symmetric:
                    pe__set_order_flags(flags, pe_order_implies_then);
                    if (pcmk__strcase_any_of(first, RSC_START, RSC_PROMOTE,
                                             NULL)) {
                        pe__set_order_flags(flags, pe_order_runnable_left);
                    }
                    break;

                case ordering_symmetric_inverse:
                    pe__set_order_flags(flags, pe_order_implies_first);
                    break;
            }
            break;
    }
    return flags;
}

/*!
 * \internal
 * \brief Find resource corresponding to ID specified in ordering
 *
 * \param[in] xml            Ordering XML
 * \param[in] resource_attr  XML attribute name for resource ID
 * \param[in] instance_attr  XML attribute name for instance number
 * \param[in] data_set       Cluster working set
 *
 * \return Resource corresponding to \p id, or NULL if none
 */
static pe_resource_t *
get_ordering_resource(xmlNode *xml, const char *resource_attr,
                      const char *instance_attr, pe_working_set_t *data_set)
{
    pe_resource_t *rsc = NULL;
    const char *rsc_id = crm_element_value(xml, resource_attr);
    const char *instance_id = crm_element_value(xml, instance_attr);

    if (rsc_id == NULL) {
        pcmk__config_err("Ignoring constraint '%s' without %s",
                         ID(xml), resource_attr);
        return NULL;
    }

    rsc = pe_find_constraint_resource(data_set->resources, rsc_id);
    if (rsc == NULL) {
        pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                         "does not exist", ID(xml), rsc_id);
        return NULL;
    }

    if (instance_id != NULL) {
        if (!pe_rsc_is_clone(rsc)) {
            pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                             "is not a clone but instance '%s' was requested",
                             ID(xml), rsc_id, instance_id);
            return NULL;
        }
        rsc = find_clone_instance(rsc, instance_id, data_set);
        if (rsc == NULL) {
            pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                             "does not have an instance '%s'",
                             "'%s'", ID(xml), rsc_id, instance_id);
            return NULL;
        }
    }
    return rsc;
}

/*!
 * \internal
 * \brief Determine minimum number of 'first' instances required in ordering
 *
 * \param[in] rsc  'First' resource in ordering
 * \param[in] xml  Ordering XML
 *
 * \return Minimum 'first' instances required (or 0 if not applicable)
 */
static int
get_minimum_first_instances(pe_resource_t *rsc, xmlNode *xml)
{
    if (pe_rsc_is_clone(rsc)) {
        const char *clone_min = NULL;

        clone_min = g_hash_table_lookup(rsc->meta,
                                        XML_RSC_ATTR_INCARNATION_MIN);
        if (clone_min != NULL) {
            int clone_min_int = 0;

            pcmk__scan_min_int(clone_min, &clone_min_int, 0);
            return clone_min_int;
        }

        /* @COMPAT 1.1.13:
         * require-all=false is deprecated equivalent of clone-min=1
         */
        clone_min = crm_element_value(xml, "require-all");
        if (clone_min != NULL) {
            pe_warn_once(pe_wo_require_all,
                         "Support for require-all in ordering constraints "
                         "is deprecated and will be removed in a future release"
                         " (use clone-min clone meta-attribute instead)");
            if (!crm_is_true(clone_min)) {
                return 1;
            }
        }
    }
    return 0;
}

/*!
 * \internal
 * \brief Create orderings for a constraint with clone-min > 0
 *
 * \param[in] id            Ordering ID
 * \param[in] rsc_first     'First' resource in ordering (a clone)
 * \param[in] action_first  'First' action in ordering
 * \param[in] rsc_then      'Then' resource in ordering
 * \param[in] action_then   'Then' action in ordering
 * \param[in] flags         Ordering flags
 * \param[in] clone_min     Minimum required instances of 'first'
 * \param[in] data_set      Cluster working set
 */
static void
clone_min_ordering(const char *id,
                   pe_resource_t *rsc_first, const char *action_first,
                   pe_resource_t *rsc_then, const char *action_then,
                   enum pe_ordering flags, int clone_min,
                   pe_working_set_t *data_set)
{
    // Create a pseudo-action for when the minimum instances are active
    char *task = crm_strdup_printf(CRM_OP_RELAXED_CLONE ":%s", id);
    pe_action_t *clone_min_met = get_pseudo_op(task, data_set);

    free(task);

    /* Require the pseudo-action to have the required number of actions to be
     * considered runnable before allowing the pseudo-action to be runnable.
     */
    clone_min_met->required_runnable_before = clone_min;
    pe__set_action_flags(clone_min_met, pe_action_requires_any);

    // Order the actions for each clone instance before the pseudo-action
    for (GList *rIter = rsc_first->children; rIter != NULL;
         rIter = rIter->next) {

        pe_resource_t *child = rIter->data;

        custom_action_order(child, pcmk__op_key(child->id, action_first, 0),
                            NULL, NULL, NULL, clone_min_met,
                            pe_order_one_or_more|pe_order_implies_then_printed,
                            data_set);
    }

    // Order "then" action after the pseudo-action (if runnable)
    custom_action_order(NULL, NULL, clone_min_met, rsc_then,
                        pcmk__op_key(rsc_then->id, action_then, 0),
                        NULL, flags|pe_order_runnable_left, data_set);
}

/*!
 * \internal
 * \brief Update ordering flags for restart-type=restart
 *
 * \param[in]  rsc    'Then' resource in ordering
 * \param[in]  kind   Ordering kind
 * \param[in]  flag   Ordering flag to set (when applicable)
 * \param[out] flags  Ordering flag set to update
 *
 * \compat The restart-type resource meta-attribute is deprecated. Eventually,
 *         it will be removed, and pe_restart_ignore will be the only behavior,
 *         at which time this can just be removed entirely.
 */
#define handle_restart_type(rsc, kind, flag, flags) do {        \
        if (((kind) == pe_order_kind_optional)                  \
            && ((rsc)->restart_type == pe_restart_restart)) {   \
            pe__set_order_flags((flags), (flag));               \
        }                                                       \
    } while (0)

/*!
 * \internal
 * \brief Create new ordering for inverse of symmetric constraint
 *
 * \param[in] id            Ordering ID (for logging only)
 * \param[in] kind          Ordering kind
 * \param[in] rsc_first     'First' resource in ordering (a clone)
 * \param[in] action_first  'First' action in ordering
 * \param[in] rsc_then      'Then' resource in ordering
 * \param[in] action_then   'Then' action in ordering
 * \param[in] data_set      Cluster working set
 */
static void
inverse_ordering(const char *id, enum pe_order_kind kind,
                 pe_resource_t *rsc_first, const char *action_first,
                 pe_resource_t *rsc_then, const char *action_then,
                 pe_working_set_t *data_set)
{
    action_then = invert_action(action_then);
    action_first = invert_action(action_first);
    if ((action_then == NULL) || (action_first == NULL)) {
        pcmk__config_warn("Cannot invert constraint '%s' "
                          "(please specify inverse manually)", id);
    } else {
        enum pe_ordering flags = ordering_flags_for_kind(kind, action_first,
                                                         ordering_symmetric_inverse);

        handle_restart_type(rsc_then, kind, pe_order_implies_first, flags);
        new_rsc_order(rsc_then, action_then, rsc_first, action_first, flags,
                      data_set);
    }
}

static void
unpack_simple_rsc_order(xmlNode * xml_obj, pe_working_set_t * data_set)
{
    pe_resource_t *rsc_then = NULL;
    pe_resource_t *rsc_first = NULL;
    int min_required_before = 0;
    enum pe_order_kind kind = pe_order_kind_mandatory;
    enum pe_ordering cons_weight = pe_order_none;
    enum ordering_symmetry symmetry;

    const char *action_then = NULL;
    const char *action_first = NULL;
    const char *id = NULL;

    CRM_CHECK(xml_obj != NULL, return);

    id = crm_element_value(xml_obj, XML_ATTR_ID);
    if (id == NULL) {
        pcmk__config_err("Ignoring <%s> constraint without " XML_ATTR_ID,
                         crm_element_name(xml_obj));
        return;
    }

    rsc_first = get_ordering_resource(xml_obj, XML_ORDER_ATTR_FIRST,
                                      XML_ORDER_ATTR_FIRST_INSTANCE,
                                      data_set);
    if (rsc_first == NULL) {
        return;
    }

    rsc_then = get_ordering_resource(xml_obj, XML_ORDER_ATTR_THEN,
                                     XML_ORDER_ATTR_THEN_INSTANCE,
                                     data_set);
    if (rsc_then == NULL) {
        return;
    }

    action_first = crm_element_value(xml_obj, XML_ORDER_ATTR_FIRST_ACTION);
    if (action_first == NULL) {
        action_first = RSC_START;
    }

    action_then = crm_element_value(xml_obj, XML_ORDER_ATTR_THEN_ACTION);
    if (action_then == NULL) {
        action_then = action_first;
    }

    kind = get_ordering_type(xml_obj);

    symmetry = get_ordering_symmetry(xml_obj, kind, NULL);
    cons_weight = ordering_flags_for_kind(kind, action_first, symmetry);

    handle_restart_type(rsc_then, kind, pe_order_implies_then, cons_weight);

    /* If there is a minimum number of instances that must be runnable before
     * the 'then' action is runnable, we use a pseudo-action for convenience:
     * minimum number of clone instances have runnable actions ->
     * pseudo-action is runnable -> dependency is runnable.
     */
    min_required_before = get_minimum_first_instances(rsc_first, xml_obj);
    if (min_required_before > 0) {
        clone_min_ordering(id, rsc_first, action_first, rsc_then, action_then,
                           cons_weight, min_required_before, data_set);
    } else {
        new_rsc_order(rsc_first, action_first, rsc_then, action_then,
                      cons_weight, data_set);
    }

    if (symmetry == ordering_symmetric) {
        inverse_ordering(id, kind, rsc_first, action_first,
                         rsc_then, action_then, data_set);
    }
}

/*!
 * \internal
 * \brief Replace any resource tags with equivalent resource_ref entries
 *
 * If a given constraint has resource sets, check each set for resource_ref
 * entries that list tags rather than resource IDs, and replace any found with
 * resource_ref entries for the corresponding resource IDs.
 *
 * \param[in]  xml_obj       Constraint XML
 * \param[in]  data_set      Cluster working set
 *
 * \return Equivalent XML with resource tags replaced (or NULL if none)
 * \note It is the caller's responsibility to free the result with free_xml().
 */
static xmlNode *
expand_tags_in_sets(xmlNode *xml_obj, pe_working_set_t *data_set)
{
    xmlNode *new_xml = NULL;
    bool any_refs = false;

    new_xml = copy_xml(xml_obj);

    for (xmlNode *set = first_named_child(new_xml, XML_CONS_TAG_RSC_SET);
         set != NULL; set = crm_next_same_xml(set)) {

        GList *tag_refs = NULL;
        GList *gIter = NULL;

        for (xmlNode *xml_rsc = first_named_child(set, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            pe_resource_t *rsc = NULL;
            pe_tag_t *tag = NULL;

            if (!valid_resource_or_tag(data_set, ID(xml_rsc), &rsc, &tag)) {
                pcmk__config_err("Ignoring resource sets for constraint '%s' "
                                 "because '%s' is not a valid resource or tag",
                                 ID(xml_obj), ID(xml_rsc));
                free_xml(new_xml);
                return NULL;

            } else if (rsc) {
                continue;

            } else if (tag) {
                /* The resource_ref under the resource_set references a template/tag */
                xmlNode *last_ref = xml_rsc;

                /* A sample:

                   Original XML:

                   <resource_set id="tag1-colocation-0" sequential="true">
                     <resource_ref id="rsc1"/>
                     <resource_ref id="tag1"/>
                     <resource_ref id="rsc4"/>
                   </resource_set>

                   Now we are appending rsc2 and rsc3 which are tagged with tag1 right after it:

                   <resource_set id="tag1-colocation-0" sequential="true">
                     <resource_ref id="rsc1"/>
                     <resource_ref id="tag1"/>
                     <resource_ref id="rsc2"/>
                     <resource_ref id="rsc3"/>
                     <resource_ref id="rsc4"/>
                   </resource_set>

                 */

                for (gIter = tag->refs; gIter != NULL; gIter = gIter->next) {
                    const char *obj_ref = (const char *) gIter->data;
                    xmlNode *new_rsc_ref = NULL;

                    new_rsc_ref = xmlNewDocRawNode(getDocPtr(set), NULL,
                                                   (pcmkXmlStr) XML_TAG_RESOURCE_REF, NULL);
                    crm_xml_add(new_rsc_ref, XML_ATTR_ID, obj_ref);
                    xmlAddNextSibling(last_ref, new_rsc_ref);

                    last_ref = new_rsc_ref;
                }

                any_refs = true;

                /* Freeing the resource_ref now would break the XML child
                 * iteration, so just remember it for freeing later.
                 */
                tag_refs = g_list_append(tag_refs, xml_rsc);
            }
        }

        /* Now free '<resource_ref id="tag1"/>', and finally get:

           <resource_set id="tag1-colocation-0" sequential="true">
             <resource_ref id="rsc1"/>
             <resource_ref id="rsc2"/>
             <resource_ref id="rsc3"/>
             <resource_ref id="rsc4"/>
           </resource_set>

         */
        for (gIter = tag_refs; gIter != NULL; gIter = gIter->next) {
            xmlNode *tag_ref = gIter->data;

            free_xml(tag_ref);
        }
        g_list_free(tag_refs);
    }

    if (!any_refs) {
        free_xml(new_xml);
        new_xml = NULL;
    }
    return new_xml;
}

static gboolean
tag_to_set(xmlNode * xml_obj, xmlNode ** rsc_set, const char * attr,
                gboolean convert_rsc, pe_working_set_t * data_set)
{
    const char *cons_id = NULL;
    const char *id = NULL;

    pe_resource_t *rsc = NULL;
    pe_tag_t *tag = NULL;

    *rsc_set = NULL;

    CRM_CHECK((xml_obj != NULL) && (attr != NULL), return FALSE);

    cons_id = ID(xml_obj);
    if (cons_id == NULL) {
        pcmk__config_err("Ignoring <%s> constraint without " XML_ATTR_ID,
                         crm_element_name(xml_obj));
        return FALSE;
    }

    id = crm_element_value(xml_obj, attr);
    if (id == NULL) {
        return TRUE;
    }

    if (valid_resource_or_tag(data_set, id, &rsc, &tag) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because '%s' is not a "
                         "valid resource or tag", cons_id, id);
        return FALSE;

    } else if (tag) {
        GList *gIter = NULL;

        /* A template/tag is referenced by the "attr" attribute (first, then, rsc or with-rsc).
           Add the template/tag's corresponding "resource_set" which contains the resources derived
           from it or tagged with it under the constraint. */
        *rsc_set = create_xml_node(xml_obj, XML_CONS_TAG_RSC_SET);
        crm_xml_add(*rsc_set, XML_ATTR_ID, id);

        for (gIter = tag->refs; gIter != NULL; gIter = gIter->next) {
            const char *obj_ref = (const char *) gIter->data;
            xmlNode *rsc_ref = NULL;

            rsc_ref = create_xml_node(*rsc_set, XML_TAG_RESOURCE_REF);
            crm_xml_add(rsc_ref, XML_ATTR_ID, obj_ref);
        }

        /* Set sequential="false" for the resource_set */
        crm_xml_add(*rsc_set, "sequential", XML_BOOLEAN_FALSE);

    } else if (rsc && convert_rsc) {
        /* Even a regular resource is referenced by "attr", convert it into a resource_set.
           Because the other side of the constraint could be a template/tag reference. */
        xmlNode *rsc_ref = NULL;

        *rsc_set = create_xml_node(xml_obj, XML_CONS_TAG_RSC_SET);
        crm_xml_add(*rsc_set, XML_ATTR_ID, id);

        rsc_ref = create_xml_node(*rsc_set, XML_TAG_RESOURCE_REF);
        crm_xml_add(rsc_ref, XML_ATTR_ID, id);

    } else {
        return TRUE;
    }

    /* Remove the "attr" attribute referencing the template/tag */
    if (*rsc_set) {
        xml_remove_prop(xml_obj, attr);
    }

    return TRUE;
}

static void unpack_rsc_location(xmlNode *xml_obj, pe_resource_t *rsc_lh,
                                const char *role, const char *score,
                                pe_working_set_t *data_set,
                                pe_re_match_data_t *match_data);

static void
unpack_simple_location(xmlNode *xml_obj, pe_working_set_t *data_set)
{
    const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
    const char *value = crm_element_value(xml_obj, XML_LOC_ATTR_SOURCE);

    if(value) {
        pe_resource_t *rsc_lh = pe_find_constraint_resource(data_set->resources, value);

        unpack_rsc_location(xml_obj, rsc_lh, NULL, NULL, data_set, NULL);
    }

    value = crm_element_value(xml_obj, XML_LOC_ATTR_SOURCE_PATTERN);
    if(value) {
        regex_t *r_patt = calloc(1, sizeof(regex_t));
        bool invert = FALSE;
        GList *rIter = NULL;

        if(value[0] == '!') {
            value++;
            invert = TRUE;
        }

        if (regcomp(r_patt, value, REG_EXTENDED)) {
            pcmk__config_err("Ignoring constraint '%s' because "
                             XML_LOC_ATTR_SOURCE_PATTERN
                             " has invalid value '%s'", id, value);
            regfree(r_patt);
            free(r_patt);
            return;
        }

        for (rIter = data_set->resources; rIter; rIter = rIter->next) {
            pe_resource_t *r = rIter->data;
            int nregs = 0;
            regmatch_t *pmatch = NULL;
            int status;

            if(r_patt->re_nsub > 0) {
                nregs = r_patt->re_nsub + 1;
            } else {
                nregs = 1;
            }
            pmatch = calloc(nregs, sizeof(regmatch_t));

            status = regexec(r_patt, r->id, nregs, pmatch, 0);

            if(invert == FALSE && status == 0) {
                pe_re_match_data_t re_match_data = {
                                                .string = r->id,
                                                .nregs = nregs,
                                                .pmatch = pmatch
                                               };

                crm_debug("'%s' matched '%s' for %s", r->id, value, id);
                unpack_rsc_location(xml_obj, r, NULL, NULL, data_set, &re_match_data);

            } else if (invert && (status != 0)) {
                crm_debug("'%s' is an inverted match of '%s' for %s", r->id, value, id);
                unpack_rsc_location(xml_obj, r, NULL, NULL, data_set, NULL);

            } else {
                crm_trace("'%s' does not match '%s' for %s", r->id, value, id);
            }

            free(pmatch);
        }

        regfree(r_patt);
        free(r_patt);
    }
}

static void
unpack_rsc_location(xmlNode *xml_obj, pe_resource_t *rsc_lh, const char *role,
                    const char *score, pe_working_set_t *data_set,
                    pe_re_match_data_t *re_match_data)
{
    pe__location_t *location = NULL;
    const char *id_lh = crm_element_value(xml_obj, XML_LOC_ATTR_SOURCE);
    const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
    const char *node = crm_element_value(xml_obj, XML_CIB_TAG_NODE);
    const char *discovery = crm_element_value(xml_obj, XML_LOCATION_ATTR_DISCOVERY);

    if (rsc_lh == NULL) {
        pcmk__config_warn("Ignoring constraint '%s' because resource '%s' "
                          "does not exist", id, id_lh);
        return;
    }

    if (score == NULL) {
        score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
    }

    if (node != NULL && score != NULL) {
        int score_i = char2score(score);
        pe_node_t *match = pe_find_node(data_set->nodes, node);

        if (!match) {
            return;
        }
        location = rsc2node_new(id, rsc_lh, score_i, discovery, match, data_set);

    } else {
        bool empty = TRUE;
        crm_time_t *next_change = crm_time_new_undefined();

        /* This loop is logically parallel to pe_evaluate_rules(), except
         * instead of checking whether any rule is active, we set up location
         * constraints for each active rule.
         */
        for (xmlNode *rule_xml = first_named_child(xml_obj, XML_TAG_RULE);
             rule_xml != NULL; rule_xml = crm_next_same_xml(rule_xml)) {
            empty = FALSE;
            crm_trace("Unpacking %s/%s", id, ID(rule_xml));
            generate_location_rule(rsc_lh, rule_xml, discovery, next_change,
                                   data_set, re_match_data);
        }

        if (empty) {
            pcmk__config_err("Ignoring constraint '%s' because it contains "
                             "no rules", id);
        }

        /* If there is a point in the future when the evaluation of a rule will
         * change, make sure the scheduler is re-run by that time.
         */
        if (crm_time_is_defined(next_change)) {
            time_t t = (time_t) crm_time_get_seconds_since_epoch(next_change);

            pe__update_recheck_time(t, data_set);
        }
        crm_time_free(next_change);
        return;
    }

    if (role == NULL) {
        role = crm_element_value(xml_obj, XML_RULE_ATTR_ROLE);
    }

    if (location && role) {
        if (text2role(role) == RSC_ROLE_UNKNOWN) {
            pe_err("Invalid constraint %s: Bad role %s", id, role);
            return;

        } else {
            enum rsc_role_e r = text2role(role);
            switch(r) {
                case RSC_ROLE_UNKNOWN:
                case RSC_ROLE_STARTED:
                case RSC_ROLE_UNPROMOTED:
                    /* Applies to all */
                    location->role_filter = RSC_ROLE_UNKNOWN;
                    break;
                default:
                    location->role_filter = r;
                    break;
            }
        }
    }
}

static gboolean
unpack_location_tags(xmlNode * xml_obj, xmlNode ** expanded_xml, pe_working_set_t * data_set)
{
    const char *id = NULL;
    const char *id_lh = NULL;
    const char *state_lh = NULL;

    pe_resource_t *rsc_lh = NULL;

    pe_tag_t *tag_lh = NULL;

    xmlNode *rsc_set_lh = NULL;

    CRM_CHECK(xml_obj != NULL, return FALSE);

    id = ID(xml_obj);
    if (id == NULL) {
        pcmk__config_err("Ignoring <%s> constraint without " XML_ATTR_ID,
                         crm_element_name(xml_obj));
        return FALSE;
    }

    // Check whether there are any resource sets with template or tag references
    *expanded_xml = expand_tags_in_sets(xml_obj, data_set);
    if (*expanded_xml != NULL) {
        crm_log_xml_trace(*expanded_xml, "Expanded rsc_location");
        return TRUE;
    }

    id_lh = crm_element_value(xml_obj, XML_LOC_ATTR_SOURCE);
    if (id_lh == NULL) {
        return TRUE;
    }

    if (valid_resource_or_tag(data_set, id_lh, &rsc_lh, &tag_lh) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because '%s' is not a "
                         "valid resource or tag", id, id_lh);
        return FALSE;

    } else if (rsc_lh) {
        /* No template is referenced. */
        return TRUE;
    }

    state_lh = crm_element_value(xml_obj, XML_RULE_ATTR_ROLE);

    *expanded_xml = copy_xml(xml_obj);

    /* Convert the template/tag reference in "rsc" into a resource_set under the rsc_location constraint. */
    if (!tag_to_set(*expanded_xml, &rsc_set_lh, XML_LOC_ATTR_SOURCE, FALSE,
                    data_set)) {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
        return FALSE;
    }

    if (rsc_set_lh) {
        if (state_lh) {
            /* A "rsc-role" is specified.
               Move it into the converted resource_set as a "role"" attribute. */
            crm_xml_add(rsc_set_lh, "role", state_lh);
            xml_remove_prop(*expanded_xml, XML_RULE_ATTR_ROLE);
        }
        crm_log_xml_trace(*expanded_xml, "Expanded rsc_location");

    } else {
        /* No sets */
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
    }

    return TRUE;
}

static gboolean
unpack_location_set(xmlNode * location, xmlNode * set, pe_working_set_t * data_set)
{
    xmlNode *xml_rsc = NULL;
    pe_resource_t *resource = NULL;
    const char *set_id;
    const char *role;
    const char *local_score;

    CRM_CHECK(set != NULL, return FALSE);

    set_id = ID(set);
    if (set_id == NULL) {
        pcmk__config_err("Ignoring " XML_CONS_TAG_RSC_SET " without "
                         XML_ATTR_ID " in constraint '%s'",
                         crm_str(ID(location)));
        return FALSE;
    }

    role = crm_element_value(set, "role");
    local_score = crm_element_value(set, XML_RULE_ATTR_SCORE);

    for (xml_rsc = first_named_child(set, XML_TAG_RESOURCE_REF);
         xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

        EXPAND_CONSTRAINT_IDREF(set_id, resource, ID(xml_rsc));
        unpack_rsc_location(location, resource, role, local_score, data_set, NULL);
    }

    return TRUE;
}

static void
unpack_location(xmlNode *xml_obj, pe_working_set_t *data_set)
{
    xmlNode *set = NULL;
    gboolean any_sets = FALSE;

    xmlNode *orig_xml = NULL;
    xmlNode *expanded_xml = NULL;

    if (unpack_location_tags(xml_obj, &expanded_xml, data_set) == FALSE) {
        return;
    }

    if (expanded_xml) {
        orig_xml = xml_obj;
        xml_obj = expanded_xml;
    }

    for (set = first_named_child(xml_obj, XML_CONS_TAG_RSC_SET);
         set != NULL; set = crm_next_same_xml(set)) {

        any_sets = TRUE;
        set = expand_idref(set, data_set->input);
        if ((set == NULL) // Configuration error, message already logged
            || !unpack_location_set(xml_obj, set, data_set)) {
            if (expanded_xml) {
                free_xml(expanded_xml);
            }
            return;
        }
    }

    if (expanded_xml) {
        free_xml(expanded_xml);
        xml_obj = orig_xml;
    }

    if (any_sets == FALSE) {
        unpack_simple_location(xml_obj, data_set);
    }
}

static int
get_node_score(const char *rule, const char *score, gboolean raw, pe_node_t * node, pe_resource_t *rsc)
{
    int score_f = 0;

    if (score == NULL) {
        pe_err("Rule %s: no score specified.  Assuming 0.", rule);

    } else if (raw) {
        score_f = char2score(score);

    } else {
        const char *attr_score = pe_node_attribute_calculated(node, score, rsc);

        if (attr_score == NULL) {
            crm_debug("Rule %s: node %s did not have a value for %s",
                      rule, node->details->uname, score);
            score_f = -INFINITY;

        } else {
            crm_debug("Rule %s: node %s had value %s for %s",
                      rule, node->details->uname, attr_score, score);
            score_f = char2score(attr_score);
        }
    }
    return score_f;
}

static pe__location_t *
generate_location_rule(pe_resource_t *rsc, xmlNode *rule_xml,
                       const char *discovery, crm_time_t *next_change,
                       pe_working_set_t *data_set,
                       pe_re_match_data_t *re_match_data)
{
    const char *rule_id = NULL;
    const char *score = NULL;
    const char *boolean = NULL;
    const char *role = NULL;

    GList *gIter = NULL;
    GList *match_L = NULL;

    gboolean do_and = TRUE;
    gboolean accept = TRUE;
    gboolean raw_score = TRUE;
    gboolean score_allocated = FALSE;

    pe__location_t *location_rule = NULL;

    rule_xml = expand_idref(rule_xml, data_set->input);
    if (rule_xml == NULL) {
        return NULL;
    }

    rule_id = crm_element_value(rule_xml, XML_ATTR_ID);
    boolean = crm_element_value(rule_xml, XML_RULE_ATTR_BOOLEAN_OP);
    role = crm_element_value(rule_xml, XML_RULE_ATTR_ROLE);

    crm_trace("Processing rule: %s", rule_id);

    if (role != NULL && text2role(role) == RSC_ROLE_UNKNOWN) {
        pe_err("Bad role specified for %s: %s", rule_id, role);
        return NULL;
    }

    score = crm_element_value(rule_xml, XML_RULE_ATTR_SCORE);
    if (score == NULL) {
        score = crm_element_value(rule_xml, XML_RULE_ATTR_SCORE_ATTRIBUTE);
        if (score != NULL) {
            raw_score = FALSE;
        }
    }
    if (pcmk__str_eq(boolean, "or", pcmk__str_casei)) {
        do_and = FALSE;
    }

    location_rule = rsc2node_new(rule_id, rsc, 0, discovery, NULL, data_set);

    if (location_rule == NULL) {
        return NULL;
    }

    if ((re_match_data != NULL) && (re_match_data->nregs > 0)
        && (re_match_data->pmatch[0].rm_so != -1) && !raw_score) {

        char *result = pe_expand_re_matches(score, re_match_data);

        if (result != NULL) {
            score = result;
            score_allocated = TRUE;
        }
    }

    if (role != NULL) {
        crm_trace("Setting role filter: %s", role);
        location_rule->role_filter = text2role(role);
        if (location_rule->role_filter == RSC_ROLE_UNPROMOTED) {
            /* Any promotable clone cannot be promoted without being in the
             * unpromoted role first. Ergo, any constraint for the unpromoted
             * role applies to every role.
             */
            location_rule->role_filter = RSC_ROLE_UNKNOWN;
        }
    }
    if (do_and) {
        GList *gIter = NULL;

        match_L = pcmk__copy_node_list(data_set->nodes, true);
        for (gIter = match_L; gIter != NULL; gIter = gIter->next) {
            pe_node_t *node = (pe_node_t *) gIter->data;

            node->weight = get_node_score(rule_id, score, raw_score, node, rsc);
        }
    }

    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        int score_f = 0;
        pe_node_t *node = (pe_node_t *) gIter->data;
        pe_match_data_t match_data = {
            .re = re_match_data,
            .params = pe_rsc_params(rsc, node, data_set),
            .meta = rsc->meta,
        };

        accept = pe_test_rule(rule_xml, node->details->attrs, RSC_ROLE_UNKNOWN,
                              data_set->now, next_change, &match_data);

        crm_trace("Rule %s %s on %s", ID(rule_xml), accept ? "passed" : "failed",
                  node->details->uname);

        score_f = get_node_score(rule_id, score, raw_score, node, rsc);

        if (accept) {
            pe_node_t *local = pe_find_node_id(match_L, node->details->id);

            if (local == NULL && do_and) {
                continue;

            } else if (local == NULL) {
                local = pe__copy_node(node);
                match_L = g_list_append(match_L, local);
            }

            if (do_and == FALSE) {
                local->weight = pe__add_scores(local->weight, score_f);
            }
            crm_trace("node %s now has weight %d", node->details->uname, local->weight);

        } else if (do_and && !accept) {
            /* remove it */
            pe_node_t *delete = pe_find_node_id(match_L, node->details->id);

            if (delete != NULL) {
                match_L = g_list_remove(match_L, delete);
                crm_trace("node %s did not match", node->details->uname);
            }
            free(delete);
        }
    }

    if (score_allocated == TRUE) {
        free((char *)score);
    }

    location_rule->node_list_rh = match_L;
    if (location_rule->node_list_rh == NULL) {
        crm_trace("No matching nodes for rule %s", rule_id);
        return NULL;
    }

    crm_trace("%s: %d nodes matched", rule_id, g_list_length(location_rule->node_list_rh));
    return location_rule;
}

static gint
sort_cons_priority_lh(gconstpointer a, gconstpointer b)
{
    const pcmk__colocation_t *rsc_constraint1 = (const pcmk__colocation_t *) a;
    const pcmk__colocation_t *rsc_constraint2 = (const pcmk__colocation_t *) b;

    if (a == NULL) {
        return 1;
    }
    if (b == NULL) {
        return -1;
    }

    CRM_ASSERT(rsc_constraint1->rsc_lh != NULL);
    CRM_ASSERT(rsc_constraint1->rsc_rh != NULL);

    if (rsc_constraint1->rsc_lh->priority > rsc_constraint2->rsc_lh->priority) {
        return -1;
    }

    if (rsc_constraint1->rsc_lh->priority < rsc_constraint2->rsc_lh->priority) {
        return 1;
    }

    /* Process clones before primitives and groups */
    if (rsc_constraint1->rsc_lh->variant > rsc_constraint2->rsc_lh->variant) {
        return -1;
    } else if (rsc_constraint1->rsc_lh->variant < rsc_constraint2->rsc_lh->variant) {
        return 1;
    }

    /* @COMPAT scheduler <2.0.0: Process promotable clones before nonpromotable
     * clones (probably unnecessary, but avoids having to update regression
     * tests)
     */
    if (rsc_constraint1->rsc_lh->variant == pe_clone) {
        if (pcmk_is_set(rsc_constraint1->rsc_lh->flags, pe_rsc_promotable)
            && !pcmk_is_set(rsc_constraint2->rsc_lh->flags, pe_rsc_promotable)) {
            return -1;
        } else if (!pcmk_is_set(rsc_constraint1->rsc_lh->flags, pe_rsc_promotable)
            && pcmk_is_set(rsc_constraint2->rsc_lh->flags, pe_rsc_promotable)) {
            return 1;
        }
    }

    return strcmp(rsc_constraint1->rsc_lh->id, rsc_constraint2->rsc_lh->id);
}

static gint
sort_cons_priority_rh(gconstpointer a, gconstpointer b)
{
    const pcmk__colocation_t *rsc_constraint1 = (const pcmk__colocation_t *) a;
    const pcmk__colocation_t *rsc_constraint2 = (const pcmk__colocation_t *) b;

    if (a == NULL) {
        return 1;
    }
    if (b == NULL) {
        return -1;
    }

    CRM_ASSERT(rsc_constraint1->rsc_lh != NULL);
    CRM_ASSERT(rsc_constraint1->rsc_rh != NULL);

    if (rsc_constraint1->rsc_rh->priority > rsc_constraint2->rsc_rh->priority) {
        return -1;
    }

    if (rsc_constraint1->rsc_rh->priority < rsc_constraint2->rsc_rh->priority) {
        return 1;
    }

    /* Process clones before primitives and groups */
    if (rsc_constraint1->rsc_rh->variant > rsc_constraint2->rsc_rh->variant) {
        return -1;
    } else if (rsc_constraint1->rsc_rh->variant < rsc_constraint2->rsc_rh->variant) {
        return 1;
    }

    /* @COMPAT scheduler <2.0.0: Process promotable clones before nonpromotable
     * clones (probably unnecessary, but avoids having to update regression
     * tests)
     */
    if (rsc_constraint1->rsc_rh->variant == pe_clone) {
        if (pcmk_is_set(rsc_constraint1->rsc_rh->flags, pe_rsc_promotable)
            && !pcmk_is_set(rsc_constraint2->rsc_rh->flags, pe_rsc_promotable)) {
            return -1;
        } else if (!pcmk_is_set(rsc_constraint1->rsc_rh->flags, pe_rsc_promotable)
            && pcmk_is_set(rsc_constraint2->rsc_rh->flags, pe_rsc_promotable)) {
            return 1;
        }
    }

    return strcmp(rsc_constraint1->rsc_rh->id, rsc_constraint2->rsc_rh->id);
}

static void
anti_colocation_order(pe_resource_t * first_rsc, int first_role,
                      pe_resource_t * then_rsc, int then_role,
                      pe_working_set_t * data_set)
{
    const char *first_tasks[] = { NULL, NULL };
    const char *then_tasks[] = { NULL, NULL };
    int first_lpc = 0;
    int then_lpc = 0;

    /* Actions to make first_rsc lose first_role */
    if (first_role == RSC_ROLE_PROMOTED) {
        first_tasks[0] = CRMD_ACTION_DEMOTE;

    } else {
        first_tasks[0] = CRMD_ACTION_STOP;

        if (first_role == RSC_ROLE_UNPROMOTED) {
            first_tasks[1] = CRMD_ACTION_PROMOTE;
        }
    }

    /* Actions to make then_rsc gain then_role */
    if (then_role == RSC_ROLE_PROMOTED) {
        then_tasks[0] = CRMD_ACTION_PROMOTE;

    } else {
        then_tasks[0] = CRMD_ACTION_START;

        if (then_role == RSC_ROLE_UNPROMOTED) {
            then_tasks[1] = CRMD_ACTION_DEMOTE;
        }
    }

    for (first_lpc = 0; first_lpc <= 1 && first_tasks[first_lpc] != NULL; first_lpc++) {
        for (then_lpc = 0; then_lpc <= 1 && then_tasks[then_lpc] != NULL; then_lpc++) {
            new_rsc_order(first_rsc, first_tasks[first_lpc], then_rsc, then_tasks[then_lpc],
                          pe_order_anti_colocation, data_set);
        }
    }
}

void
pcmk__new_colocation(const char *id, const char *node_attr, int score,
                     pe_resource_t *rsc_lh, pe_resource_t *rsc_rh,
                     const char *state_lh, const char *state_rh,
                     bool influence, pe_working_set_t *data_set)
{
    pcmk__colocation_t *new_con = NULL;

    if (score == 0) {
        crm_trace("Ignoring colocation '%s' because score is 0", id);
        return;
    }
    if ((rsc_lh == NULL) || (rsc_rh == NULL)) {
        pcmk__config_err("Ignoring colocation '%s' because resource "
                         "does not exist", id);
        return;
    }

    new_con = calloc(1, sizeof(pcmk__colocation_t));
    if (new_con == NULL) {
        return;
    }

    if (pcmk__str_eq(state_lh, RSC_ROLE_STARTED_S, pcmk__str_null_matches | pcmk__str_casei)) {
        state_lh = RSC_ROLE_UNKNOWN_S;
    }

    if (pcmk__str_eq(state_rh, RSC_ROLE_STARTED_S, pcmk__str_null_matches | pcmk__str_casei)) {
        state_rh = RSC_ROLE_UNKNOWN_S;
    }

    new_con->id = id;
    new_con->rsc_lh = rsc_lh;
    new_con->rsc_rh = rsc_rh;
    new_con->score = score;
    new_con->role_lh = text2role(state_lh);
    new_con->role_rh = text2role(state_rh);
    new_con->node_attribute = node_attr;
    new_con->influence = influence;

    if (node_attr == NULL) {
        node_attr = CRM_ATTR_UNAME;
    }

    pe_rsc_trace(rsc_lh, "%s ==> %s (%s %d)", rsc_lh->id, rsc_rh->id, node_attr, score);

    rsc_lh->rsc_cons = g_list_insert_sorted(rsc_lh->rsc_cons, new_con, sort_cons_priority_rh);

    rsc_rh->rsc_cons_lhs =
        g_list_insert_sorted(rsc_rh->rsc_cons_lhs, new_con, sort_cons_priority_lh);

    data_set->colocation_constraints = g_list_append(data_set->colocation_constraints, new_con);

    if (score <= -INFINITY) {
        anti_colocation_order(rsc_lh, new_con->role_lh, rsc_rh, new_con->role_rh, data_set);
        anti_colocation_order(rsc_rh, new_con->role_rh, rsc_lh, new_con->role_lh, data_set);
    }
}

/* LHS before RHS */
int
new_rsc_order(pe_resource_t * lh_rsc, const char *lh_task,
              pe_resource_t * rh_rsc, const char *rh_task,
              enum pe_ordering type, pe_working_set_t * data_set)
{
    char *lh_key = NULL;
    char *rh_key = NULL;

    CRM_CHECK(lh_rsc != NULL, return -1);
    CRM_CHECK(lh_task != NULL, return -1);
    CRM_CHECK(rh_rsc != NULL, return -1);
    CRM_CHECK(rh_task != NULL, return -1);

    lh_key = pcmk__op_key(lh_rsc->id, lh_task, 0);
    rh_key = pcmk__op_key(rh_rsc->id, rh_task, 0);

    return custom_action_order(lh_rsc, lh_key, NULL, rh_rsc, rh_key, NULL, type, data_set);
}

static char *
task_from_action_or_key(pe_action_t *action, const char *key)
{
    char *res = NULL;

    if (action) {
        res = strdup(action->task);
    } else if (key) {
        parse_op_key(key, NULL, &res, NULL);
    }
    return res;
}

/* when order constraints are made between two resources start and stop actions
 * those constraints have to be mirrored against the corresponding
 * migration actions to ensure start/stop ordering is preserved during
 * a migration */
static void
handle_migration_ordering(pe__ordering_t *order, pe_working_set_t *data_set)
{
    char *lh_task = NULL;
    char *rh_task = NULL;
    gboolean rh_migratable;
    gboolean lh_migratable;

    if (order->lh_rsc == NULL || order->rh_rsc == NULL) {
        return;
    } else if (order->lh_rsc == order->rh_rsc) {
        return;
    /* don't mess with those constraints built between parent
     * resources and the children */
    } else if (is_parent(order->lh_rsc, order->rh_rsc)) {
        return;
    } else if (is_parent(order->rh_rsc, order->lh_rsc)) {
        return;
    }

    lh_migratable = pcmk_is_set(order->lh_rsc->flags, pe_rsc_allow_migrate);
    rh_migratable = pcmk_is_set(order->rh_rsc->flags, pe_rsc_allow_migrate);

    /* one of them has to be migratable for
     * the migrate ordering logic to be applied */
    if (lh_migratable == FALSE && rh_migratable == FALSE) {
        return;
    }

    /* at this point we have two resources which allow migrations that have an
     * order dependency set between them.  If those order dependencies involve
     * start/stop actions, we need to mirror the corresponding migrate actions
     * so order will be preserved. */
    lh_task = task_from_action_or_key(order->lh_action, order->lh_action_task);
    rh_task = task_from_action_or_key(order->rh_action, order->rh_action_task);
    if (lh_task == NULL || rh_task == NULL) {
        goto cleanup_order;
    }

    if (pcmk__str_eq(lh_task, RSC_START, pcmk__str_casei) && pcmk__str_eq(rh_task, RSC_START, pcmk__str_casei)) {
        int flags = pe_order_optional;

        if (lh_migratable && rh_migratable) {
            /* A start then B start
             * A migrate_from then B migrate_to */
            custom_action_order(order->lh_rsc,
                                pcmk__op_key(order->lh_rsc->id, RSC_MIGRATED, 0),
                                NULL, order->rh_rsc,
                                pcmk__op_key(order->rh_rsc->id, RSC_MIGRATE, 0),
                                NULL, flags, data_set);
        }

        if (rh_migratable) {
            if (lh_migratable) {
                pe__set_order_flags(flags, pe_order_apply_first_non_migratable);
            }

            /* A start then B start
             * A start then B migrate_to... only if A start is not a part of a migration*/
            custom_action_order(order->lh_rsc,
                                pcmk__op_key(order->lh_rsc->id, RSC_START, 0),
                                NULL, order->rh_rsc,
                                pcmk__op_key(order->rh_rsc->id, RSC_MIGRATE, 0),
                                NULL, flags, data_set);
        }

    } else if (rh_migratable == TRUE && pcmk__str_eq(lh_task, RSC_STOP, pcmk__str_casei) && pcmk__str_eq(rh_task, RSC_STOP, pcmk__str_casei)) {
        int flags = pe_order_optional;

        if (lh_migratable) {
            pe__set_order_flags(flags, pe_order_apply_first_non_migratable);
        }

        /* rh side is at the bottom of the stack during a stop. If we have a constraint
         * stop B then stop A, if B is migrating via stop/start, and A is migrating using migration actions,
         * we need to enforce that A's migrate_to action occurs after B's stop action. */
        custom_action_order(order->lh_rsc,
                            pcmk__op_key(order->lh_rsc->id, RSC_STOP, 0), NULL,
                            order->rh_rsc,
                            pcmk__op_key(order->rh_rsc->id, RSC_MIGRATE, 0),
                            NULL, flags, data_set);

        /* We need to build the stop constraint against migrate_from as well
         * to account for partial migrations. */
        if (order->rh_rsc->partial_migration_target) {
            custom_action_order(order->lh_rsc,
                                pcmk__op_key(order->lh_rsc->id, RSC_STOP, 0),
                                NULL, order->rh_rsc,
                                pcmk__op_key(order->rh_rsc->id, RSC_MIGRATED, 0),
                                NULL, flags, data_set);
        }

    } else if (pcmk__str_eq(lh_task, RSC_PROMOTE, pcmk__str_casei) && pcmk__str_eq(rh_task, RSC_START, pcmk__str_casei)) {
        int flags = pe_order_optional;

        if (rh_migratable) {
            /* A promote then B start
             * A promote then B migrate_to */
            custom_action_order(order->lh_rsc,
                                pcmk__op_key(order->lh_rsc->id, RSC_PROMOTE, 0),
                                NULL, order->rh_rsc,
                                pcmk__op_key(order->rh_rsc->id, RSC_MIGRATE, 0),
                                NULL, flags, data_set);
        }

    } else if (pcmk__str_eq(lh_task, RSC_DEMOTE, pcmk__str_casei) && pcmk__str_eq(rh_task, RSC_STOP, pcmk__str_casei)) {
        int flags = pe_order_optional;

        if (rh_migratable) {
            /* A demote then B stop
             * A demote then B migrate_to */
            custom_action_order(order->lh_rsc, pcmk__op_key(order->lh_rsc->id, RSC_DEMOTE, 0), NULL,
                                order->rh_rsc, pcmk__op_key(order->rh_rsc->id, RSC_MIGRATE, 0), NULL,
                                flags, data_set);

            /* We need to build the demote constraint against migrate_from as well
             * to account for partial migrations. */
            if (order->rh_rsc->partial_migration_target) {
                custom_action_order(order->lh_rsc,
                                    pcmk__op_key(order->lh_rsc->id, RSC_DEMOTE, 0),
                                    NULL, order->rh_rsc,
                                    pcmk__op_key(order->rh_rsc->id, RSC_MIGRATED, 0),
                                    NULL, flags, data_set);
            }
        }
    }

cleanup_order:
    free(lh_task);
    free(rh_task);
}

/* LHS before RHS */
int
custom_action_order(pe_resource_t * lh_rsc, char *lh_action_task, pe_action_t * lh_action,
                    pe_resource_t * rh_rsc, char *rh_action_task, pe_action_t * rh_action,
                    enum pe_ordering type, pe_working_set_t * data_set)
{
    pe__ordering_t *order = NULL;

    if (lh_rsc == NULL && lh_action) {
        lh_rsc = lh_action->rsc;
    }
    if (rh_rsc == NULL && rh_action) {
        rh_rsc = rh_action->rsc;
    }

    if ((lh_action == NULL && lh_rsc == NULL)
        || (rh_action == NULL && rh_rsc == NULL)) {
        crm_err("Invalid ordering (bug?)");
        free(lh_action_task);
        free(rh_action_task);
        return -1;
    }

    order = calloc(1, sizeof(pe__ordering_t));

    order->id = data_set->order_id++;
    order->type = type;
    order->lh_rsc = lh_rsc;
    order->rh_rsc = rh_rsc;
    order->lh_action = lh_action;
    order->rh_action = rh_action;
    order->lh_action_task = lh_action_task;
    order->rh_action_task = rh_action_task;

    if (order->lh_action_task == NULL && lh_action) {
        order->lh_action_task = strdup(lh_action->uuid);
    }

    if (order->rh_action_task == NULL && rh_action) {
        order->rh_action_task = strdup(rh_action->uuid);
    }

    if (order->lh_rsc == NULL && lh_action) {
        order->lh_rsc = lh_action->rsc;
    }

    if (order->rh_rsc == NULL && rh_action) {
        order->rh_rsc = rh_action->rsc;
    }

    pe_rsc_trace(lh_rsc, "Created ordering %d for %s then %s",
                 (data_set->order_id - 1),
                 ((lh_action_task == NULL)? "?" : lh_action_task),
                 ((rh_action_task == NULL)? "?" : rh_action_task));

    data_set->ordering_constraints = g_list_prepend(data_set->ordering_constraints, order);
    handle_migration_ordering(order, data_set);

    return order->id;
}

static gboolean
unpack_order_set(xmlNode * set, enum pe_order_kind parent_kind, pe_resource_t ** rsc,
                 const char *parent_symmetrical_s,
                 pe_working_set_t * data_set)
{
    xmlNode *xml_rsc = NULL;
    GList *set_iter = NULL;
    GList *resources = NULL;

    pe_resource_t *last = NULL;
    pe_resource_t *resource = NULL;

    int local_kind = parent_kind;
    gboolean sequential = FALSE;
    enum pe_ordering flags = pe_order_optional;
    enum ordering_symmetry symmetry;

    char *key = NULL;
    const char *id = ID(set);
    const char *action = crm_element_value(set, "action");
    const char *sequential_s = crm_element_value(set, "sequential");
    const char *kind_s = crm_element_value(set, XML_ORDER_ATTR_KIND);

    if (action == NULL) {
        action = RSC_START;
    }

    if (kind_s) {
        local_kind = get_ordering_type(set);
    }
    if (sequential_s == NULL) {
        sequential_s = "1";
    }

    sequential = crm_is_true(sequential_s);

    symmetry = get_ordering_symmetry(set, parent_kind, parent_symmetrical_s);
    flags = ordering_flags_for_kind(local_kind, action, symmetry);

    for (xml_rsc = first_named_child(set, XML_TAG_RESOURCE_REF);
         xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

        EXPAND_CONSTRAINT_IDREF(id, resource, ID(xml_rsc));
        resources = g_list_append(resources, resource);
    }

    if (pcmk__list_of_1(resources)) {
        crm_trace("Single set: %s", id);
        *rsc = resource;
        goto done;
    }

    *rsc = NULL;

    set_iter = resources;
    while (set_iter != NULL) {
        resource = (pe_resource_t *) set_iter->data;
        set_iter = set_iter->next;

        key = pcmk__op_key(resource->id, action, 0);

        if (local_kind == pe_order_kind_serialize) {
            /* Serialize before everything that comes after */

            GList *gIter = NULL;

            for (gIter = set_iter; gIter != NULL; gIter = gIter->next) {
                pe_resource_t *then_rsc = (pe_resource_t *) gIter->data;
                char *then_key = pcmk__op_key(then_rsc->id, action, 0);

                custom_action_order(resource, strdup(key), NULL, then_rsc, then_key, NULL,
                                    flags, data_set);
            }

        } else if (sequential) {
            if (last != NULL) {
                new_rsc_order(last, action, resource, action, flags, data_set);
            }
            last = resource;
        }
        free(key);
    }

    if (symmetry == ordering_asymmetric) {
        goto done;
    }

    last = NULL;
    action = invert_action(action);

    flags = ordering_flags_for_kind(local_kind, action,
                                    ordering_symmetric_inverse);

    set_iter = resources;
    while (set_iter != NULL) {
        resource = (pe_resource_t *) set_iter->data;
        set_iter = set_iter->next;

        if (sequential) {
            if (last != NULL) {
                new_rsc_order(resource, action, last, action, flags, data_set);
            }
            last = resource;
        }
    }

  done:
    g_list_free(resources);
    return TRUE;
}

static gboolean
order_rsc_sets(const char *id, xmlNode * set1, xmlNode * set2, enum pe_order_kind kind,
               pe_working_set_t *data_set, enum ordering_symmetry symmetry)
{

    xmlNode *xml_rsc = NULL;
    xmlNode *xml_rsc_2 = NULL;

    pe_resource_t *rsc_1 = NULL;
    pe_resource_t *rsc_2 = NULL;

    const char *action_1 = crm_element_value(set1, "action");
    const char *action_2 = crm_element_value(set2, "action");

    const char *sequential_1 = crm_element_value(set1, "sequential");
    const char *sequential_2 = crm_element_value(set2, "sequential");

    const char *require_all_s = crm_element_value(set1, "require-all");
    gboolean require_all = require_all_s ? crm_is_true(require_all_s) : TRUE;

    enum pe_ordering flags = pe_order_none;

    if (action_1 == NULL) {
        action_1 = RSC_START;
    };

    if (action_2 == NULL) {
        action_2 = RSC_START;
    };

    if (symmetry == ordering_symmetric_inverse) {
        action_1 = invert_action(action_1);
        action_2 = invert_action(action_2);
    }

    if(pcmk__str_eq(RSC_STOP, action_1, pcmk__str_casei) || pcmk__str_eq(RSC_DEMOTE, action_1, pcmk__str_casei)) {
        /* Assuming: A -> ( B || C) -> D
         * The one-or-more logic only applies during the start/promote phase
         * During shutdown neither B nor can shutdown until D is down, so simply turn require_all back on.
         */
        require_all = TRUE;
    }

    // @TODO is action_2 correct here?
    flags = ordering_flags_for_kind(kind, action_2, symmetry);

    /* If we have an un-ordered set1, whether it is sequential or not is irrelevant in regards to set2. */
    if (!require_all) {
        char *task = crm_strdup_printf(CRM_OP_RELAXED_SET ":%s", ID(set1));
        pe_action_t *unordered_action = get_pseudo_op(task, data_set);

        free(task);
        pe__set_action_flags(unordered_action, pe_action_requires_any);

        for (xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));

            /* Add an ordering constraint between every element in set1 and the pseudo action.
             * If any action in set1 is runnable the pseudo action will be runnable. */
            custom_action_order(rsc_1, pcmk__op_key(rsc_1->id, action_1, 0),
                                NULL, NULL, NULL, unordered_action,
                                pe_order_one_or_more|pe_order_implies_then_printed,
                                data_set);
        }
        for (xml_rsc_2 = first_named_child(set2, XML_TAG_RESOURCE_REF);
             xml_rsc_2 != NULL; xml_rsc_2 = crm_next_same_xml(xml_rsc_2)) {

            EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc_2));

            /* Add an ordering constraint between the pseudo action and every element in set2.
             * If the pseudo action is runnable, every action in set2 will be runnable */
            custom_action_order(NULL, NULL, unordered_action,
                                rsc_2, pcmk__op_key(rsc_2->id, action_2, 0),
                                NULL, flags|pe_order_runnable_left, data_set);
        }

        return TRUE;
    }

    if (crm_is_true(sequential_1)) {
        if (symmetry == ordering_symmetric_inverse) {
            /* get the first one */
            xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
            if (xml_rsc != NULL) {
                EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
            }

        } else {
            /* get the last one */
            const char *rid = NULL;

            for (xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
                 xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {
                rid = ID(xml_rsc);
            }
            EXPAND_CONSTRAINT_IDREF(id, rsc_1, rid);
        }
    }

    if (crm_is_true(sequential_2)) {
        if (symmetry == ordering_symmetric_inverse) {
            /* get the last one */
            const char *rid = NULL;

            for (xml_rsc = first_named_child(set2, XML_TAG_RESOURCE_REF);
                 xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

                rid = ID(xml_rsc);
            }
            EXPAND_CONSTRAINT_IDREF(id, rsc_2, rid);

        } else {
            /* get the first one */
            xml_rsc = first_named_child(set2, XML_TAG_RESOURCE_REF);
            if (xml_rsc != NULL) {
                EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc));
            }
        }
    }

    if (rsc_1 != NULL && rsc_2 != NULL) {
        new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);

    } else if (rsc_1 != NULL) {
        for (xml_rsc = first_named_child(set2, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc));
            new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);
        }

    } else if (rsc_2 != NULL) {
        xmlNode *xml_rsc = NULL;

        for (xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
            new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);
        }

    } else {
        for (xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            xmlNode *xml_rsc_2 = NULL;

            EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));

            for (xml_rsc_2 = first_named_child(set2, XML_TAG_RESOURCE_REF);
                 xml_rsc_2 != NULL; xml_rsc_2 = crm_next_same_xml(xml_rsc_2)) {

                EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc_2));
                new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);
            }
        }
    }

    return TRUE;
}

/*!
 * \internal
 * \brief If an ordering constraint uses resource tags, expand them
 *
 * \param[in]  xml_obj       Ordering constraint XML
 * \param[out] expanded_xml  Equivalent XML with tags expanded
 * \param[in]  data_set      Cluster working set
 *
 * \return Standard Pacemaker return code (specifically, pcmk_rc_ok on success,
 *         and pcmk_rc_schema_validation on invalid configuration)
 */
static int
unpack_order_tags(xmlNode *xml_obj, xmlNode **expanded_xml,
                  pe_working_set_t *data_set)
{
    const char *id_first = NULL;
    const char *id_then = NULL;
    const char *action_first = NULL;
    const char *action_then = NULL;

    pe_resource_t *rsc_first = NULL;
    pe_resource_t *rsc_then = NULL;
    pe_tag_t *tag_first = NULL;
    pe_tag_t *tag_then = NULL;

    xmlNode *rsc_set_first = NULL;
    xmlNode *rsc_set_then = NULL;
    gboolean any_sets = FALSE;

    // Check whether there are any resource sets with template or tag references
    *expanded_xml = expand_tags_in_sets(xml_obj, data_set);
    if (*expanded_xml != NULL) {
        crm_log_xml_trace(*expanded_xml, "Expanded rsc_order");
        return pcmk_rc_ok;
    }

    id_first = crm_element_value(xml_obj, XML_ORDER_ATTR_FIRST);
    id_then = crm_element_value(xml_obj, XML_ORDER_ATTR_THEN);
    if (id_first == NULL || id_then == NULL) {
        return pcmk_rc_ok;
    }

    if (valid_resource_or_tag(data_set, id_first, &rsc_first, &tag_first) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because '%s' is not a "
                         "valid resource or tag", ID(xml_obj), id_first);
        return pcmk_rc_schema_validation;
    }

    if (valid_resource_or_tag(data_set, id_then, &rsc_then, &tag_then) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because '%s' is not a "
                         "valid resource or tag", ID(xml_obj), id_then);
        return pcmk_rc_schema_validation;
    }

    if (rsc_first && rsc_then) {
        /* Neither side references any template/tag. */
        return pcmk_rc_ok;
    }

    action_first = crm_element_value(xml_obj, XML_ORDER_ATTR_FIRST_ACTION);
    action_then = crm_element_value(xml_obj, XML_ORDER_ATTR_THEN_ACTION);

    *expanded_xml = copy_xml(xml_obj);

    /* Convert the template/tag reference in "first" into a resource_set under the order constraint. */
    if (!tag_to_set(*expanded_xml, &rsc_set_first, XML_ORDER_ATTR_FIRST, TRUE,
                    data_set)) {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
        return pcmk_rc_schema_validation;
    }

    if (rsc_set_first) {
        if (action_first) {
            /* A "first-action" is specified.
               Move it into the converted resource_set as an "action" attribute. */
            crm_xml_add(rsc_set_first, "action", action_first);
            xml_remove_prop(*expanded_xml, XML_ORDER_ATTR_FIRST_ACTION);
        }
        any_sets = TRUE;
    }

    /* Convert the template/tag reference in "then" into a resource_set under the order constraint. */
    if (!tag_to_set(*expanded_xml, &rsc_set_then, XML_ORDER_ATTR_THEN, TRUE,
                    data_set)) {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
        return pcmk_rc_schema_validation;
    }

    if (rsc_set_then) {
        if (action_then) {
            /* A "then-action" is specified.
               Move it into the converted resource_set as an "action" attribute. */
            crm_xml_add(rsc_set_then, "action", action_then);
            xml_remove_prop(*expanded_xml, XML_ORDER_ATTR_THEN_ACTION);
        }
        any_sets = TRUE;
    }

    if (any_sets) {
        crm_log_xml_trace(*expanded_xml, "Expanded rsc_order");
    } else {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
    }

    return pcmk_rc_ok;
}

/*!
 * \internal
 * \brief Unpack ordering constraint XML
 *
 * \param[in]     xml_obj   Ordering constraint XML to unpack
 * \param[in,out] data_set  Cluster working set
 */
static void
unpack_rsc_order(xmlNode *xml_obj, pe_working_set_t *data_set)
{
    pe_resource_t *rsc = NULL;

    xmlNode *set = NULL;
    xmlNode *last = NULL;

    xmlNode *orig_xml = NULL;
    xmlNode *expanded_xml = NULL;

    const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
    const char *invert = crm_element_value(xml_obj, XML_CONS_ATTR_SYMMETRICAL);
    enum pe_order_kind kind = get_ordering_type(xml_obj);

    enum ordering_symmetry symmetry = get_ordering_symmetry(xml_obj, kind, NULL);

    // Expand any resource tags in the constraint XML
    if (unpack_order_tags(xml_obj, &expanded_xml, data_set) != pcmk_rc_ok) {
        return;
    }
    if (expanded_xml != NULL) {
        orig_xml = xml_obj;
        xml_obj = expanded_xml;
    }

    // If the constraint has resource sets, unpack them
    for (set = first_named_child(xml_obj, XML_CONS_TAG_RSC_SET); set != NULL;
         set = crm_next_same_xml(set)) {

        set = expand_idref(set, data_set->input);
        if ((set == NULL) // Configuration error, message already logged
            || !unpack_order_set(set, kind, &rsc, invert, data_set)) {
            if (expanded_xml != NULL) {
                free_xml(expanded_xml);
            }
            return;
        }

        if ((last != NULL)
            && (!order_rsc_sets(id, last, set, kind, data_set, symmetry)
                || ((symmetry == ordering_symmetric)
                    && !order_rsc_sets(id, set, last, kind, data_set,
                                       ordering_symmetric_inverse)))) {
            if (expanded_xml != NULL) {
                free_xml(expanded_xml);
            }
            return;
        }
        last = set;
    }

    if (expanded_xml) {
        free_xml(expanded_xml);
        xml_obj = orig_xml;
    }

    // If the constraint has no resource sets, unpack it as a simple ordering
    if (last == NULL) {
        unpack_simple_rsc_order(xml_obj, data_set);
    }
}

/*!
 * \internal
 * \brief Return the boolean influence corresponding to configuration
 *
 * \param[in] coloc_id     Colocation XML ID (for error logging)
 * \param[in] rsc          Resource involved in constraint (for default)
 * \param[in] influence_s  String value of influence option
 *
 * \return true if string evaluates true, false if string evaluates false,
 *         or value of resource's critical option if string is NULL or invalid
 */
static bool
unpack_influence(const char *coloc_id, const pe_resource_t *rsc,
                 const char *influence_s)
{
    if (influence_s != NULL) {
        int influence_i = 0;

        if (crm_str_to_boolean(influence_s, &influence_i) < 0) {
            pcmk__config_err("Constraint '%s' has invalid value for "
                             XML_COLOC_ATTR_INFLUENCE " (using default)",
                             coloc_id);
        } else {
            return (influence_i == TRUE);
        }
    }
    return pcmk_is_set(rsc->flags, pe_rsc_critical);
}

static gboolean
unpack_colocation_set(xmlNode *set, int score, const char *coloc_id,
                      const char *influence_s, pe_working_set_t *data_set)
{
    xmlNode *xml_rsc = NULL;
    pe_resource_t *with = NULL;
    pe_resource_t *resource = NULL;
    const char *set_id = ID(set);
    const char *role = crm_element_value(set, "role");
    const char *sequential = crm_element_value(set, "sequential");
    const char *ordering = crm_element_value(set, "ordering");
    int local_score = score;

    const char *score_s = crm_element_value(set, XML_RULE_ATTR_SCORE);

    if (score_s) {
        local_score = char2score(score_s);
    }
    if (local_score == 0) {
        crm_trace("Ignoring colocation '%s' for set '%s' because score is 0",
                  coloc_id, set_id);
        return TRUE;
    }

    if(ordering == NULL) {
        ordering = "group";
    }

    if (sequential != NULL && crm_is_true(sequential) == FALSE) {
        return TRUE;

    } else if ((local_score > 0)
               && pcmk__str_eq(ordering, "group", pcmk__str_casei)) {
        for (xml_rsc = first_named_child(set, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            EXPAND_CONSTRAINT_IDREF(set_id, resource, ID(xml_rsc));
            if (with != NULL) {
                pe_rsc_trace(resource, "Colocating %s with %s", resource->id, with->id);
                pcmk__new_colocation(set_id, NULL, local_score, resource,
                                     with, role, role,
                                     unpack_influence(coloc_id, resource,
                                                      influence_s),
                                     data_set);
            }
            with = resource;
        }
    } else if (local_score > 0) {
        pe_resource_t *last = NULL;
        for (xml_rsc = first_named_child(set, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            EXPAND_CONSTRAINT_IDREF(set_id, resource, ID(xml_rsc));
            if (last != NULL) {
                pe_rsc_trace(resource, "Colocating %s with %s", last->id, resource->id);
                pcmk__new_colocation(set_id, NULL, local_score, last,
                                     resource, role, role,
                                     unpack_influence(coloc_id, last,
                                                      influence_s),
                                     data_set);
            }

            last = resource;
        }

    } else {
        /* Anti-colocating with every prior resource is
         * the only way to ensure the intuitive result
         * (i.e. that no one in the set can run with anyone else in the set)
         */

        for (xml_rsc = first_named_child(set, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            xmlNode *xml_rsc_with = NULL;
            bool influence = true;

            EXPAND_CONSTRAINT_IDREF(set_id, resource, ID(xml_rsc));
            influence = unpack_influence(coloc_id, resource, influence_s);

            for (xml_rsc_with = first_named_child(set, XML_TAG_RESOURCE_REF);
                 xml_rsc_with != NULL;
                 xml_rsc_with = crm_next_same_xml(xml_rsc_with)) {

                if (pcmk__str_eq(resource->id, ID(xml_rsc_with), pcmk__str_casei)) {
                    break;
                }
                EXPAND_CONSTRAINT_IDREF(set_id, with, ID(xml_rsc_with));
                pe_rsc_trace(resource, "Anti-Colocating %s with %s", resource->id,
                             with->id);
                pcmk__new_colocation(set_id, NULL, local_score,
                                     resource, with, role, role,
                                     influence, data_set);
            }
        }
    }

    return TRUE;
}

static gboolean
colocate_rsc_sets(const char *id, xmlNode * set1, xmlNode * set2, int score,
                  const char *influence_s, pe_working_set_t *data_set)
{
    xmlNode *xml_rsc = NULL;
    pe_resource_t *rsc_1 = NULL;
    pe_resource_t *rsc_2 = NULL;

    const char *role_1 = crm_element_value(set1, "role");
    const char *role_2 = crm_element_value(set2, "role");

    const char *sequential_1 = crm_element_value(set1, "sequential");
    const char *sequential_2 = crm_element_value(set2, "sequential");

    if (score == 0) {
        crm_trace("Ignoring colocation '%s' between sets because score is 0",
                  id);
        return TRUE;
    }
    if (sequential_1 == NULL || crm_is_true(sequential_1)) {
        /* get the first one */
        xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
        if (xml_rsc != NULL) {
            EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
        }
    }

    if (sequential_2 == NULL || crm_is_true(sequential_2)) {
        /* get the last one */
        const char *rid = NULL;

        for (xml_rsc = first_named_child(set2, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {
            rid = ID(xml_rsc);
        }
        EXPAND_CONSTRAINT_IDREF(id, rsc_2, rid);
    }

    if (rsc_1 != NULL && rsc_2 != NULL) {
        pcmk__new_colocation(id, NULL, score, rsc_1, rsc_2, role_1, role_2,
                             unpack_influence(id, rsc_1, influence_s),
                             data_set);

    } else if (rsc_1 != NULL) {
        bool influence = unpack_influence(id, rsc_1, influence_s);

        for (xml_rsc = first_named_child(set2, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc));
            pcmk__new_colocation(id, NULL, score, rsc_1, rsc_2, role_1,
                                 role_2, influence, data_set);
        }

    } else if (rsc_2 != NULL) {
        for (xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
            pcmk__new_colocation(id, NULL, score, rsc_1, rsc_2, role_1,
                                 role_2,
                                 unpack_influence(id, rsc_1, influence_s),
                                 data_set);
        }

    } else {
        for (xml_rsc = first_named_child(set1, XML_TAG_RESOURCE_REF);
             xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

            xmlNode *xml_rsc_2 = NULL;
            bool influence = true;

            EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
            influence = unpack_influence(id, rsc_1, influence_s);

            for (xml_rsc_2 = first_named_child(set2, XML_TAG_RESOURCE_REF);
                 xml_rsc_2 != NULL; xml_rsc_2 = crm_next_same_xml(xml_rsc_2)) {

                EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc_2));
                pcmk__new_colocation(id, NULL, score, rsc_1, rsc_2,
                                     role_1, role_2, influence,
                                     data_set);
            }
        }
    }

    return TRUE;
}

static void
unpack_simple_colocation(xmlNode *xml_obj, const char *id,
                         const char *influence_s, pe_working_set_t *data_set)
{
    int score_i = 0;

    const char *score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
    const char *id_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE);
    const char *id_rh = crm_element_value(xml_obj, XML_COLOC_ATTR_TARGET);
    const char *state_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE_ROLE);
    const char *state_rh = crm_element_value(xml_obj, XML_COLOC_ATTR_TARGET_ROLE);
    const char *attr = crm_element_value(xml_obj, XML_COLOC_ATTR_NODE_ATTR);
    const char *symmetrical = crm_element_value(xml_obj, XML_CONS_ATTR_SYMMETRICAL);

    // experimental syntax from pacemaker-next (unlikely to be adopted as-is)
    const char *instance_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE_INSTANCE);
    const char *instance_rh = crm_element_value(xml_obj, XML_COLOC_ATTR_TARGET_INSTANCE);

    pe_resource_t *rsc_lh = pe_find_constraint_resource(data_set->resources, id_lh);
    pe_resource_t *rsc_rh = pe_find_constraint_resource(data_set->resources, id_rh);

    if (rsc_lh == NULL) {
        pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                         "does not exist", id, id_lh);
        return;

    } else if (rsc_rh == NULL) {
        pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                         "does not exist", id, id_rh);
        return;

    } else if (instance_lh && pe_rsc_is_clone(rsc_lh) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                         "is not a clone but instance '%s' was requested",
                         id, id_lh, instance_lh);
        return;

    } else if (instance_rh && pe_rsc_is_clone(rsc_rh) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                         "is not a clone but instance '%s' was requested",
                         id, id_rh, instance_rh);
        return;
    }

    if (instance_lh) {
        rsc_lh = find_clone_instance(rsc_lh, instance_lh, data_set);
        if (rsc_lh == NULL) {
            pcmk__config_warn("Ignoring constraint '%s' because resource '%s' "
                              "does not have an instance '%s'",
                              id, id_lh, instance_lh);
            return;
        }
    }

    if (instance_rh) {
        rsc_rh = find_clone_instance(rsc_rh, instance_rh, data_set);
        if (rsc_rh == NULL) {
            pcmk__config_warn("Ignoring constraint '%s' because resource '%s' "
                              "does not have an instance '%s'",
                              "'%s'", id, id_rh, instance_rh);
            return;
        }
    }

    if (crm_is_true(symmetrical)) {
        pcmk__config_warn("The colocation constraint '"
                          XML_CONS_ATTR_SYMMETRICAL
                          "' attribute has been removed");
    }

    if (score) {
        score_i = char2score(score);
    }

    pcmk__new_colocation(id, attr, score_i, rsc_lh, rsc_rh, state_lh, state_rh,
                         unpack_influence(id, rsc_lh, influence_s), data_set);
}

static gboolean
unpack_colocation_tags(xmlNode * xml_obj, xmlNode ** expanded_xml, pe_working_set_t * data_set)
{
    const char *id = NULL;
    const char *id_lh = NULL;
    const char *id_rh = NULL;
    const char *state_lh = NULL;
    const char *state_rh = NULL;

    pe_resource_t *rsc_lh = NULL;
    pe_resource_t *rsc_rh = NULL;

    pe_tag_t *tag_lh = NULL;
    pe_tag_t *tag_rh = NULL;

    xmlNode *rsc_set_lh = NULL;
    xmlNode *rsc_set_rh = NULL;
    gboolean any_sets = FALSE;

    *expanded_xml = NULL;

    CRM_CHECK(xml_obj != NULL, return FALSE);

    id = ID(xml_obj);
    if (id == NULL) {
        pcmk__config_err("Ignoring <%s> constraint without " XML_ATTR_ID,
                         crm_element_name(xml_obj));
        return FALSE;
    }

    // Check whether there are any resource sets with template or tag references
    *expanded_xml = expand_tags_in_sets(xml_obj, data_set);
    if (*expanded_xml != NULL) {
        crm_log_xml_trace(*expanded_xml, "Expanded rsc_colocation");
        return TRUE;
    }

    id_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE);
    id_rh = crm_element_value(xml_obj, XML_COLOC_ATTR_TARGET);
    if (id_lh == NULL || id_rh == NULL) {
        return TRUE;
    }

    if (valid_resource_or_tag(data_set, id_lh, &rsc_lh, &tag_lh) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because '%s' is not a "
                         "valid resource or tag", id, id_lh);
        return FALSE;
    }

    if (valid_resource_or_tag(data_set, id_rh, &rsc_rh, &tag_rh) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because '%s' is not a "
                         "valid resource or tag", id, id_rh);
        return FALSE;
    }

    if (rsc_lh && rsc_rh) {
        /* Neither side references any template/tag. */
        return TRUE;
    }

    if (tag_lh && tag_rh) {
        /* A colocation constraint between two templates/tags makes no sense. */
        pcmk__config_err("Ignoring constraint '%s' because two templates or "
                         "tags cannot be colocated", id);
        return FALSE;
    }

    state_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE_ROLE);
    state_rh = crm_element_value(xml_obj, XML_COLOC_ATTR_TARGET_ROLE);

    *expanded_xml = copy_xml(xml_obj);

    /* Convert the template/tag reference in "rsc" into a resource_set under the colocation constraint. */
    if (!tag_to_set(*expanded_xml, &rsc_set_lh, XML_COLOC_ATTR_SOURCE, TRUE,
                    data_set)) {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
        return FALSE;
    }

    if (rsc_set_lh) {
        if (state_lh) {
            /* A "rsc-role" is specified.
               Move it into the converted resource_set as a "role"" attribute. */
            crm_xml_add(rsc_set_lh, "role", state_lh);
            xml_remove_prop(*expanded_xml, XML_COLOC_ATTR_SOURCE_ROLE);
        }
        any_sets = TRUE;
    }

    /* Convert the template/tag reference in "with-rsc" into a resource_set under the colocation constraint. */
    if (!tag_to_set(*expanded_xml, &rsc_set_rh, XML_COLOC_ATTR_TARGET, TRUE,
                    data_set)) {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
        return FALSE;
    }

    if (rsc_set_rh) {
        if (state_rh) {
            /* A "with-rsc-role" is specified.
               Move it into the converted resource_set as a "role"" attribute. */
            crm_xml_add(rsc_set_rh, "role", state_rh);
            xml_remove_prop(*expanded_xml, XML_COLOC_ATTR_TARGET_ROLE);
        }
        any_sets = TRUE;
    }

    if (any_sets) {
        crm_log_xml_trace(*expanded_xml, "Expanded rsc_colocation");
    } else {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
    }

    return TRUE;
}

static void
unpack_rsc_colocation(xmlNode *xml_obj, pe_working_set_t *data_set)
{
    int score_i = 0;
    xmlNode *set = NULL;
    xmlNode *last = NULL;

    xmlNode *orig_xml = NULL;
    xmlNode *expanded_xml = NULL;

    const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
    const char *score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
    const char *influence_s = crm_element_value(xml_obj,
                                                XML_COLOC_ATTR_INFLUENCE);

    if (score) {
        score_i = char2score(score);
    }

    if (!unpack_colocation_tags(xml_obj, &expanded_xml, data_set)) {
        return;
    }
    if (expanded_xml) {
        orig_xml = xml_obj;
        xml_obj = expanded_xml;
    }

    for (set = first_named_child(xml_obj, XML_CONS_TAG_RSC_SET); set != NULL;
         set = crm_next_same_xml(set)) {

        set = expand_idref(set, data_set->input);
        if ((set == NULL) // Configuration error, message already logged
            || !unpack_colocation_set(set, score_i, id, influence_s, data_set)
            || ((last != NULL) && !colocate_rsc_sets(id, last, set, score_i,
                                                     influence_s, data_set))) {
            if (expanded_xml != NULL) {
                free_xml(expanded_xml);
            }
            return;
        }
        last = set;
    }

    if (expanded_xml) {
        free_xml(expanded_xml);
        xml_obj = orig_xml;
    }

    if (last == NULL) {
        unpack_simple_colocation(xml_obj, id, influence_s, data_set);
    }
}

static void
rsc_ticket_new(const char *id, pe_resource_t *rsc_lh, pe_ticket_t *ticket,
               const char *state_lh, const char *loss_policy,
               pe_working_set_t *data_set)
{
    rsc_ticket_t *new_rsc_ticket = NULL;

    if (rsc_lh == NULL) {
        pcmk__config_err("Ignoring ticket '%s' because resource "
                         "does not exist", id);
        return;
    }

    new_rsc_ticket = calloc(1, sizeof(rsc_ticket_t));
    if (new_rsc_ticket == NULL) {
        return;
    }

    if (pcmk__str_eq(state_lh, RSC_ROLE_STARTED_S, pcmk__str_null_matches | pcmk__str_casei)) {
        state_lh = RSC_ROLE_UNKNOWN_S;
    }

    new_rsc_ticket->id = id;
    new_rsc_ticket->ticket = ticket;
    new_rsc_ticket->rsc_lh = rsc_lh;
    new_rsc_ticket->role_lh = text2role(state_lh);

    if (pcmk__str_eq(loss_policy, "fence", pcmk__str_casei)) {
        if (pcmk_is_set(data_set->flags, pe_flag_stonith_enabled)) {
            new_rsc_ticket->loss_policy = loss_ticket_fence;
        } else {
            pcmk__config_err("Resetting '" XML_TICKET_ATTR_LOSS_POLICY
                             "' for ticket '%s' to 'stop' "
                             "because fencing is not configured", ticket->id);
            loss_policy = "stop";
        }
    }

    if (new_rsc_ticket->loss_policy == loss_ticket_fence) {
        crm_debug("On loss of ticket '%s': Fence the nodes running %s (%s)",
                  new_rsc_ticket->ticket->id, new_rsc_ticket->rsc_lh->id,
                  role2text(new_rsc_ticket->role_lh));

    } else if (pcmk__str_eq(loss_policy, "freeze", pcmk__str_casei)) {
        crm_debug("On loss of ticket '%s': Freeze %s (%s)",
                  new_rsc_ticket->ticket->id, new_rsc_ticket->rsc_lh->id,
                  role2text(new_rsc_ticket->role_lh));
        new_rsc_ticket->loss_policy = loss_ticket_freeze;

    } else if (pcmk__str_eq(loss_policy, "demote", pcmk__str_casei)) {
        crm_debug("On loss of ticket '%s': Demote %s (%s)",
                  new_rsc_ticket->ticket->id, new_rsc_ticket->rsc_lh->id,
                  role2text(new_rsc_ticket->role_lh));
        new_rsc_ticket->loss_policy = loss_ticket_demote;

    } else if (pcmk__str_eq(loss_policy, "stop", pcmk__str_casei)) {
        crm_debug("On loss of ticket '%s': Stop %s (%s)",
                  new_rsc_ticket->ticket->id, new_rsc_ticket->rsc_lh->id,
                  role2text(new_rsc_ticket->role_lh));
        new_rsc_ticket->loss_policy = loss_ticket_stop;

    } else {
        if (new_rsc_ticket->role_lh == RSC_ROLE_PROMOTED) {
            crm_debug("On loss of ticket '%s': Default to demote %s (%s)",
                      new_rsc_ticket->ticket->id, new_rsc_ticket->rsc_lh->id,
                      role2text(new_rsc_ticket->role_lh));
            new_rsc_ticket->loss_policy = loss_ticket_demote;

        } else {
            crm_debug("On loss of ticket '%s': Default to stop %s (%s)",
                      new_rsc_ticket->ticket->id, new_rsc_ticket->rsc_lh->id,
                      role2text(new_rsc_ticket->role_lh));
            new_rsc_ticket->loss_policy = loss_ticket_stop;
        }
    }

    pe_rsc_trace(rsc_lh, "%s (%s) ==> %s", rsc_lh->id, role2text(new_rsc_ticket->role_lh),
                 ticket->id);

    rsc_lh->rsc_tickets = g_list_append(rsc_lh->rsc_tickets, new_rsc_ticket);

    data_set->ticket_constraints = g_list_append(data_set->ticket_constraints, new_rsc_ticket);

    if (new_rsc_ticket->ticket->granted == FALSE || new_rsc_ticket->ticket->standby) {
        rsc_ticket_constraint(rsc_lh, new_rsc_ticket, data_set);
    }
}

static gboolean
unpack_rsc_ticket_set(xmlNode * set, pe_ticket_t * ticket, const char *loss_policy,
                      pe_working_set_t * data_set)
{
    xmlNode *xml_rsc = NULL;
    pe_resource_t *resource = NULL;
    const char *set_id = NULL;
    const char *role = NULL;

    CRM_CHECK(set != NULL, return FALSE);
    CRM_CHECK(ticket != NULL, return FALSE);

    set_id = ID(set);
    if (set_id == NULL) {
        pcmk__config_err("Ignoring <" XML_CONS_TAG_RSC_SET "> without "
                         XML_ATTR_ID);
        return FALSE;
    }

    role = crm_element_value(set, "role");

    for (xml_rsc = first_named_child(set, XML_TAG_RESOURCE_REF);
         xml_rsc != NULL; xml_rsc = crm_next_same_xml(xml_rsc)) {

        EXPAND_CONSTRAINT_IDREF(set_id, resource, ID(xml_rsc));
        pe_rsc_trace(resource, "Resource '%s' depends on ticket '%s'",
                     resource->id, ticket->id);
        rsc_ticket_new(set_id, resource, ticket, role, loss_policy, data_set);
    }

    return TRUE;
}

static void
unpack_simple_rsc_ticket(xmlNode * xml_obj, pe_working_set_t * data_set)
{
    const char *id = NULL;
    const char *ticket_str = crm_element_value(xml_obj, XML_TICKET_ATTR_TICKET);
    const char *loss_policy = crm_element_value(xml_obj, XML_TICKET_ATTR_LOSS_POLICY);

    pe_ticket_t *ticket = NULL;

    const char *id_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE);
    const char *state_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE_ROLE);

    // experimental syntax from pacemaker-next (unlikely to be adopted as-is)
    const char *instance_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE_INSTANCE);

    pe_resource_t *rsc_lh = NULL;

    CRM_CHECK(xml_obj != NULL, return);

    id = ID(xml_obj);
    if (id == NULL) {
        pcmk__config_err("Ignoring <%s> constraint without " XML_ATTR_ID,
                         crm_element_name(xml_obj));
        return;
    }

    if (ticket_str == NULL) {
        pcmk__config_err("Ignoring constraint '%s' without ticket specified",
                         id);
        return;
    } else {
        ticket = g_hash_table_lookup(data_set->tickets, ticket_str);
    }

    if (ticket == NULL) {
        pcmk__config_err("Ignoring constraint '%s' because ticket '%s' "
                         "does not exist", id, ticket_str);
        return;
    }

    if (id_lh == NULL) {
        pcmk__config_err("Ignoring constraint '%s' without resource", id);
        return;
    } else {
        rsc_lh = pe_find_constraint_resource(data_set->resources, id_lh);
    }

    if (rsc_lh == NULL) {
        pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                         "does not exist", id, id_lh);
        return;

    } else if (instance_lh && pe_rsc_is_clone(rsc_lh) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because resource '%s' "
                         "is not a clone but instance '%s' was requested",
                         id, id_lh, instance_lh);
        return;
    }

    if (instance_lh) {
        rsc_lh = find_clone_instance(rsc_lh, instance_lh, data_set);
        if (rsc_lh == NULL) {
            pcmk__config_warn("Ignoring constraint '%s' because resource '%s' "
                              "does not have an instance '%s'",
                              "'%s'", id, id_lh, instance_lh);
            return;
        }
    }

    rsc_ticket_new(id, rsc_lh, ticket, state_lh, loss_policy, data_set);
}

static gboolean
unpack_rsc_ticket_tags(xmlNode * xml_obj, xmlNode ** expanded_xml, pe_working_set_t * data_set)
{
    const char *id = NULL;
    const char *id_lh = NULL;
    const char *state_lh = NULL;

    pe_resource_t *rsc_lh = NULL;
    pe_tag_t *tag_lh = NULL;

    xmlNode *rsc_set_lh = NULL;

    CRM_CHECK(xml_obj != NULL, return FALSE);

    id = ID(xml_obj);
    if (id == NULL) {
        pcmk__config_err("Ignoring <%s> constraint without " XML_ATTR_ID,
                         crm_element_name(xml_obj));
        return FALSE;
    }

    // Check whether there are any resource sets with template or tag references
    *expanded_xml = expand_tags_in_sets(xml_obj, data_set);
    if (*expanded_xml != NULL) {
        crm_log_xml_trace(*expanded_xml, "Expanded rsc_ticket");
        return TRUE;
    }

    id_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE);
    if (id_lh == NULL) {
        return TRUE;
    }

    if (valid_resource_or_tag(data_set, id_lh, &rsc_lh, &tag_lh) == FALSE) {
        pcmk__config_err("Ignoring constraint '%s' because '%s' is not a "
                         "valid resource or tag", id, id_lh);
        return FALSE;

    } else if (rsc_lh) {
        /* No template/tag is referenced. */
        return TRUE;
    }

    state_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE_ROLE);

    *expanded_xml = copy_xml(xml_obj);

    /* Convert the template/tag reference in "rsc" into a resource_set under the rsc_ticket constraint. */
    if (!tag_to_set(*expanded_xml, &rsc_set_lh, XML_COLOC_ATTR_SOURCE, FALSE,
                    data_set)) {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
        return FALSE;
    }

    if (rsc_set_lh) {
        if (state_lh) {
            /* A "rsc-role" is specified.
               Move it into the converted resource_set as a "role"" attribute. */
            crm_xml_add(rsc_set_lh, "role", state_lh);
            xml_remove_prop(*expanded_xml, XML_COLOC_ATTR_SOURCE_ROLE);
        }
    } else {
        free_xml(*expanded_xml);
        *expanded_xml = NULL;
    }

    return TRUE;
}

static void
unpack_rsc_ticket(xmlNode *xml_obj, pe_working_set_t *data_set)
{
    xmlNode *set = NULL;
    gboolean any_sets = FALSE;

    const char *id = NULL;
    const char *ticket_str = crm_element_value(xml_obj, XML_TICKET_ATTR_TICKET);
    const char *loss_policy = crm_element_value(xml_obj, XML_TICKET_ATTR_LOSS_POLICY);

    pe_ticket_t *ticket = NULL;

    xmlNode *orig_xml = NULL;
    xmlNode *expanded_xml = NULL;

    gboolean rc = TRUE;

    CRM_CHECK(xml_obj != NULL, return);

    id = ID(xml_obj);
    if (id == NULL) {
        pcmk__config_err("Ignoring <%s> constraint without " XML_ATTR_ID,
                         crm_element_name(xml_obj));
        return;
    }

    if (data_set->tickets == NULL) {
        data_set->tickets = pcmk__strkey_table(free, destroy_ticket);
    }

    if (ticket_str == NULL) {
        pcmk__config_err("Ignoring constraint '%s' without ticket", id);
        return;
    } else {
        ticket = g_hash_table_lookup(data_set->tickets, ticket_str);
    }

    if (ticket == NULL) {
        ticket = ticket_new(ticket_str, data_set);
        if (ticket == NULL) {
            return;
        }
    }

    rc = unpack_rsc_ticket_tags(xml_obj, &expanded_xml, data_set);
    if (expanded_xml) {
        orig_xml = xml_obj;
        xml_obj = expanded_xml;

    } else if (rc == FALSE) {
        return;
    }

    for (set = first_named_child(xml_obj, XML_CONS_TAG_RSC_SET); set != NULL;
         set = crm_next_same_xml(set)) {

        any_sets = TRUE;
        set = expand_idref(set, data_set->input);
        if ((set == NULL) // Configuration error, message already logged
            || !unpack_rsc_ticket_set(set, ticket, loss_policy, data_set)) {
            if (expanded_xml != NULL) {
                free_xml(expanded_xml);
            }
            return;
        }
    }

    if (expanded_xml) {
        free_xml(expanded_xml);
        xml_obj = orig_xml;
    }

    if (any_sets == FALSE) {
        unpack_simple_rsc_ticket(xml_obj, data_set);
    }
}
