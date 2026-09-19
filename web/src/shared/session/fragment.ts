/**
 * The API's Google callback redirects to `/#qm_auth=<jwt>` (or
 * `/#qm_auth_error=<reason>`). A fragment is never sent to a server, so the
 * token does not land in access logs; the app reads it once and strips it from
 * the URL so it does not stay in the history or get copied from the address bar.
 */
export type AuthFragment = { token?: string; error?: string };

export function parseAuthFragment(hash: string): AuthFragment | null {
	const params = new URLSearchParams(hash.replace(/^#/, ""));
	const token = params.get("qm_auth");
	const error = params.get("qm_auth_error");
	if (token) return { token };
	if (error) return { error };
	return null;
}
