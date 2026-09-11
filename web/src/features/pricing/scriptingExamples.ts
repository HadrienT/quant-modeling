/** Example scripts for ScriptingPreview — kept out of the component so its
 * own file stays under the 230-line feature-file cap (WP 07/11). */
export const SCRIPTING_EXAMPLES: Record<string, string> = {
	"European call": "2027-09-10\n    pays max(spot() - 100, 0)\n",
	"Arithmetic Asian":
		"2026-12-10\n    acc = spot()\n\n" +
		"2027-03-10  2027-06-10\n    acc = acc + spot()\n\n" +
		"2027-09-10\n    acc = acc + spot()\n    pays max(acc / 4 - 100, 0)\n",
	"Digital (try fuzzy)":
		"2027-09-10\n    if spot() > 100 then pays 100 endIf\n",
	"Memory autocall":
		"2026-12-10\n    s0 = spot()\n    miss = 0\n    alive = 1\n\n" +
		"2027-06-10  2027-12-10\n" +
		"    if alive = 1 then\n" +
		"        if spot() >= s0 then\n" +
		"            pays 1000 * (1 + 0.05 * (miss + 1))\n" +
		"            alive = 0\n" +
		"        else\n" +
		"            if spot() >= 0.70 * s0 then\n" +
		"                pays 1000 * 0.05 * (miss + 1)\n" +
		"                miss = 0\n" +
		"            else\n" +
		"                miss = miss + 1\n" +
		"            endIf\n" +
		"        endIf\n" +
		"    endIf\n\n" +
		"2028-06-10\n" +
		"    if alive = 1 then\n" +
		"        if spot() < 0.60 * s0 then pays 1000 * spot() / s0\n" +
		"        else pays 1000 endIf\n" +
		"    endIf\n",
};
