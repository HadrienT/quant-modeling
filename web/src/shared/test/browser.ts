import { setupWorker } from "msw/browser";
import { handlers } from "./handlers";

/** Browser worker — used by Storybook and for offline `npm run dev`. */
export const worker = setupWorker(...handlers);
