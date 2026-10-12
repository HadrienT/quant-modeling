import { useState } from "react";
import {
	type XvaSensitivitiesRequest,
	useXvaSensitivities,
} from "@/shared/api";
import { SensitivitiesPanel } from "@/shared/xva";

/** The sensitivities of one of the page's portfolios, asked for on demand. */
export function XvaSensitivities({
	request,
}: {
	request: XvaSensitivitiesRequest;
}) {
	const [asked, setAsked] = useState(false);
	const query = useXvaSensitivities(request, asked);
	return (
		<SensitivitiesPanel
			query={query}
			asked={asked}
			onAsk={() => setAsked(true)}
		/>
	);
}
