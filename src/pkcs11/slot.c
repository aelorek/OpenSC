/*
 * slot.c: Smartcard and slot related management functions
 *
 * Copyright (C) 2002  Timo Ter�s <timo.teras@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>
#include "sc-pkcs11.h"

static struct sc_pkcs11_framework_ops *frameworks[] = {
        &framework_pkcs15,
#ifdef USE_PKCS15_INIT
	/* This should be the last framework, because it
	 * will assume the card is blank and try to initialize it */
        &framework_pkcs15init,
#endif
	NULL
};

static void init_slot_info(CK_SLOT_INFO_PTR pInfo)
{
	strcpy_bp(pInfo->slotDescription, "Virtual slot", 64);
	strcpy_bp(pInfo->manufacturerID, "OpenSC project (www.opensc.org)", 32);
	pInfo->flags = CKF_REMOVABLE_DEVICE | CKF_HW_SLOT;
	pInfo->hardwareVersion.major = 0;
	pInfo->hardwareVersion.minor = 0;
	pInfo->firmwareVersion.major = 0;
        pInfo->firmwareVersion.minor = 0;
}

CK_RV card_initialize(int reader)
{
	memset(&card_table[reader], 0, sizeof(struct sc_pkcs11_card));
	card_table[reader].reader = reader;
        return CKR_OK;
}

static CK_RV card_detect(int reader)
{
	struct sc_pkcs11_card *card;
        int rc, rv, i, retry = 1;

        rv = CKR_OK;

	debug(context, "%d: Detecting SmartCard\n", reader);

	/* Check if someone inserted a card */
again:	rc = sc_detect_card_presence(context->reader[reader], 0);
	if (rc < 0) {
		debug(context, "Card detection failed for reader %d: %s\n",
				reader, sc_strerror(rc));
		return sc_to_cryptoki_error(rc, reader);
	}
	if (rc == 0) {
		debug(context, "%d: Card absent\n", reader);
		card_removed(reader); /* Release all resources */
		return CKR_TOKEN_NOT_PRESENT;
	}

	/* If the card was changed, disconnect the current one */
	if (rc & SC_SLOT_CARD_CHANGED) {
		debug(context, "%d: Card changed\n", reader);
		/* The following should never happen - but if it
		 * does we'll be stuck in an endless loop.
		 * So better be fussy. */
		if (!retry--)
			return CKR_TOKEN_NOT_PRESENT;
		card_removed(reader);
		goto again;
	}

	/* Detect the card if it's not known already */
	if (card_table[reader].card == NULL) {
		debug(context, "%d: Connecting to SmartCard\n", reader);
		rc = sc_connect_card(context->reader[reader], 0, &card_table[reader].card);
		if (rc != SC_SUCCESS)
			return sc_to_cryptoki_error(rc, reader);
	}

	/* Detect the framework */
	if (card_table[reader].framework == NULL) {
		debug(context, "%d: Detecting Framework\n", reader);

		card = &card_table[reader];

		if (sc_pkcs11_conf.num_slots == 0)
			card->max_slots = SC_PKCS11_DEF_SLOTS_PER_CARD;
		else
			card->max_slots = sc_pkcs11_conf.num_slots;
		card->num_slots = 0;

		for (i = 0; frameworks[i]; i++) {
			if (frameworks[i]->bind == NULL)
				continue;
			rv = frameworks[i]->bind(card);
			if (rv == CKR_OK)
				break;
		}

		if (frameworks[i] == NULL)
			return CKR_TOKEN_NOT_RECOGNIZED;

		/* Initialize framework */
		debug(context, "%d: Detected framework %d. Creating tokens.\n", reader, i);
		rv = frameworks[i]->create_tokens(card);
		if (rv != CKR_OK)
                        return rv;

		card_table[reader].framework = frameworks[i];
	}

	debug(context, "%d: Detection ended\n", reader);
	return rv;
}

CK_RV card_detect_all(void)
{
	int i;

	if (context == NULL_PTR)
		return CKR_CRYPTOKI_NOT_INITIALIZED;
	for (i = 0; i < context->reader_count; i++)
		card_detect(i);

	return CKR_OK;
}

CK_RV card_removed(int reader)
{
	int i;
        struct sc_pkcs11_card *card;

	debug(context, "%d: SmartCard removed\n", reader);

	for (i=0; i<SC_PKCS11_MAX_VIRTUAL_SLOTS; i++) {
		if (virtual_slots[i].card &&
		    virtual_slots[i].card->reader == reader)
                        slot_token_removed(i);
	}

	card = &card_table[reader];
	if (card->framework)
		card->framework->unbind(card);
	card->framework = NULL;
	card->fw_data = NULL;

	if (card->card)
		sc_disconnect_card(card->card, 0);
        card->card = NULL;

        return CKR_OK;
}

CK_RV slot_initialize(int id, struct sc_pkcs11_slot *slot)
{
        memset(slot, 0, sizeof(slot));
	slot->id = id;
	slot->login_user = -1;
        init_slot_info(&slot->slot_info);
	pool_initialize(&slot->object_pool, POOL_TYPE_OBJECT);

        return CKR_OK;
}

CK_RV slot_allocate(struct sc_pkcs11_slot **slot, struct sc_pkcs11_card *card)
{
        int i;

	if (card->num_slots >= card->max_slots)
		return CKR_FUNCTION_FAILED;

	for (i=0; i<SC_PKCS11_MAX_VIRTUAL_SLOTS; i++) {
		if (!virtual_slots[i].card) {
			debug(context, "Allocated slot %d\n", i);

                        virtual_slots[i].card = card;
			*slot = &virtual_slots[i];
			strcpy_bp((*slot)->slot_info.slotDescription,
				card->card->reader->name, 64);
			card->num_slots++;
			return CKR_OK;
		}
	}
        return CKR_FUNCTION_FAILED;

}

CK_RV slot_get_slot(int id, struct sc_pkcs11_slot **slot)
{
	if (context == NULL)
                return CKR_CRYPTOKI_NOT_INITIALIZED;

	if (id < 0 || id >= SC_PKCS11_MAX_VIRTUAL_SLOTS)
		return CKR_SLOT_ID_INVALID;

        *slot = &virtual_slots[id];
        return CKR_OK;
}

CK_RV slot_get_token(int id, struct sc_pkcs11_slot **slot)
{
	int rv;

	rv = slot_get_slot(id, slot);
	if (rv != CKR_OK)
		return rv;

	if (!((*slot)->slot_info.flags & CKF_TOKEN_PRESENT))
		return CKR_TOKEN_NOT_PRESENT;

        return CKR_OK;
}

CK_RV slot_token_removed(int id)
{
	int rv;
        struct sc_pkcs11_slot *slot;
        struct sc_pkcs11_object *object;

	rv = slot_get_slot(id, &slot);
	if (rv != CKR_OK)
		return rv;

        /* Terminate active sessions */
        C_CloseAllSessions(id);

	/* Object pool */
	while (pool_find_and_delete(&slot->object_pool, 0, (void**) &object) == CKR_OK) {
                if (object->ops->release)
			object->ops->release(object);
	}

	/* Release framework stuff */
	if (slot->card != NULL && slot->fw_data != NULL)
		slot->card->framework->release_token(slot->card, slot->fw_data);

        /* Zap everything else */
	memset(slot, 0, sizeof(*slot));
        init_slot_info(&slot->slot_info);
	slot->login_user = -1;

        return CKR_OK;

}
