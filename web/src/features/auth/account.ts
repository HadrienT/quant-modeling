type Account = { username: string; email?: string | null };

/** What to call the user: the email for Google accounts (their username is
 * the opaque `google:<sub>`), the username otherwise. */
export function displayName(me: Account): string {
	return me.email ?? me.username;
}

/** One letter for the avatar. */
export function initial(me: Account): string {
	return displayName(me).charAt(0).toUpperCase() || "?";
}
