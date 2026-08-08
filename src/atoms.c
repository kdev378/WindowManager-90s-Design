/*
 * atoms.c - アトムテーブルの一括 intern と _NET_SUPPORTED の組み立て
 *
 * 対応する仕様: docs/SPEC.md §5.2.1
 */
#include <stdlib.h>

#include "w98wm.h"

xcb_atom_t atoms[ATOM_COUNT];

/*
 * すべてのアトムを 1 往復で intern する。
 * まず全リクエストを送ってから返信を回収する（逐次 send→wait は禁止）。
 */
void atoms_init(xcb_connection_t *conn)
{
	xcb_intern_atom_cookie_t cookies[ATOM_COUNT];

#define X(id, sup, str) \
	cookies[ATOM_##id] = xcb_intern_atom(conn, 0, \
	    (uint16_t)(sizeof(str) - 1), str);
	ATOM_LIST(X)
#undef X

	for (size_t i = 0; i < ATOM_COUNT; i++) {
		xcb_intern_atom_reply_t *reply =
		    xcb_intern_atom_reply(conn, cookies[i], NULL);
		if (reply != NULL) {
			atoms[i] = reply->atom;
			free(reply);
		} else {
			atoms[i] = XCB_ATOM_NONE;
		}
	}
}

/*
 * _NET_SUPPORTED に載せるアトムを X-macro の ATOM_SUP フラグから機械的に組み立てる。
 * SPEC §5.2.1: 実装・テストが揃っていないアトムを載せない規約を守るため、
 * このリストは atoms.h を編集するだけで自動的に追従する。
 */
uint32_t atoms_supported_list(xcb_atom_t *out, uint32_t max)
{
	uint32_t n = 0;

#define X(id, sup, str) \
	if ((sup) == ATOM_SUP) { \
		if (n < max) \
			out[n] = atoms[ATOM_##id]; \
		n++; \
	}
	ATOM_LIST(X)
#undef X

	return n;
}

/* アトム値から enum index への逆引き。線形探索（ATOM_COUNT は約 90） */
int atoms_lookup(xcb_atom_t a)
{
	if (a == XCB_ATOM_NONE)
		return -1;

	for (int i = 0; i < ATOM_COUNT; i++) {
		if (atoms[i] == a)
			return i;
	}
	return -1;
}
