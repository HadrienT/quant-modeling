import { useState } from "react";
import { readLocalPortfolios } from "@/shared/portfolio";
import { Button } from "@/shared/ui";

/**
 * Anonymous-portfolio notice + first-login migration offer (WP 04 §4).
 * "These portfolios are on this device only" is always shown; the migration
 * prompt appears once a session exists.
 */
export function MigrationBanner() {
	const [dismissed, setDismissed] = useState(false);
	const local = readLocalPortfolios();
	if (dismissed || local.length === 0) return null;

	return (
		<div className="flex items-center justify-between gap-3 rounded-md border border-hairline bg-surface-raised px-3 py-2 text-xs text-ink-secondary">
			<span>
				These {local.length} portfolio(s) live on this device only. Sign in to
				keep them across devices — you'll be offered to migrate them.
			</span>
			<Button size="sm" variant="ghost" onClick={() => setDismissed(true)}>
				Got it
			</Button>
		</div>
	);
}
