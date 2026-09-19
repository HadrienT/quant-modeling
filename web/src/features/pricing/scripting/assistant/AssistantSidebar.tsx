import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/shared/ui";
import { LanguageReference } from "../LanguageReference";
import { AssistantPanel } from "./AssistantPanel";
import type { EditorContext } from "./useAssistantChat";

/** Right-hand column of the scripting page: the assistant, or the static
 * language reference. Both stay mounted so a chat survives a tab switch. */
export function AssistantSidebar(props: {
	script: string;
	error: string | null;
	valuationDate: string;
	dayCount: EditorContext["day_count"];
	onScript: (script: string) => void;
}) {
	return (
		<Tabs defaultValue="assistant" className="min-w-0">
			<TabsList>
				<TabsTrigger value="assistant">Assistant</TabsTrigger>
				<TabsTrigger value="reference">Reference</TabsTrigger>
			</TabsList>
			<TabsContent
				value="assistant"
				forceMount
				className="data-[state=inactive]:hidden"
			>
				<AssistantPanel {...props} />
			</TabsContent>
			<TabsContent
				value="reference"
				forceMount
				className="data-[state=inactive]:hidden"
			>
				<LanguageReference />
			</TabsContent>
		</Tabs>
	);
}
