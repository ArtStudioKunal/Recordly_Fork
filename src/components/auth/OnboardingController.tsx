import { useEffect, useRef } from "react";
import type { User } from "@supabase/supabase-js";

/**
 * Local-first fork: startup does not require a cloud account.
 *
 * We intentionally do not create a fake Supabase user or access token.
 * Cloud-dependent operations continue to require real authentication, while
 * screen recording and editing are available without signing in.
 */
export function OnboardingController({
  ready,
}: {
  requestNonce: number;
  ready: boolean;
  user: User | null;
  configured: boolean;
  callbackError?: string;
}) {
  const launched = useRef(false);

  useEffect(() => {
    if (!ready || launched.current) return;
    launched.current = true;
    void window.electronAPI.showRecordingHud().catch((error) => {
      console.warn("Could not open recording controls:", error);
      launched.current = false;
    });
  }, [ready]);

  // No cloud sign-in/onboarding modal in the local recording build.
  return null;
}
