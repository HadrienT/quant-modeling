import { Info } from "lucide-react";
import type { PositionMark } from "@/shared/api";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/shared/ui";
import { ModelDetails } from "./ModelDetails";

/** The i on a position: hover or focus it to see the model that priced it. */
export function ModelTooltip({ position: p }: { position: PositionMark }) {
	return (
		<Tooltip delayDuration={150}>
			<TooltipTrigger asChild>
				<button
					type="button"
					aria-label={`Model used for ${p.label}`}
					className="rounded-full text-ink-muted hover:text-accent focus-visible:text-accent"
				>
					<Info className="size-3.5" />
				</button>
			</TooltipTrigger>
			<TooltipContent side="right" align="start" className="max-w-md p-3">
				<ModelDetails position={p} />
			</TooltipContent>
		</Tooltip>
	);
}
