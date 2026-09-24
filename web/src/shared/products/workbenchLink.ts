/**
 * The pricing workbench keeps its form values in the URL (`?p=`, base64url
 * JSON), so any page can open it pre-filled — e.g. the FX tab's "price a
 * quanto with these inputs". Values are in the form's units (percent for
 * rates and vols).
 */
export function encodeParams(obj: unknown): string {
	return btoa(unescape(encodeURIComponent(JSON.stringify(obj))))
		.replace(/\+/g, "-")
		.replace(/\//g, "_")
		.replace(/=+$/, "");
}

export function decodeParams<T>(s: string | undefined, fallback: T): T {
	if (!s) return fallback;
	try {
		const b = s.replace(/-/g, "+").replace(/_/g, "/");
		return JSON.parse(decodeURIComponent(escape(atob(b)))) as T;
	} catch {
		return fallback;
	}
}
