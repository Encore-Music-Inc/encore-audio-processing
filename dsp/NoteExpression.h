#pragma once

// How one note is played, beyond its pitch and velocity: what a score adds to
// a note (its dynamics shaping it, a slur, a glide, a technique's colour). The
// synthesizer (Synth.h) and the sampler's instruments (Sampler.h) both take it.

namespace vs
{
    struct NoteExpression
    {
        float gain = 1.0f;          // on the note's level
        float brightness = 1.0f;    // on the tone (the synth's note-following cutoff; a recording's low-pass)
        bool slurred = false;       // starts at slurLevel of the top, attack at least slurAttack, no bursts
        float slurLevel = 0.35f, slurAttack = 0.03f;
        float bendSemitones = 0.0f; // a glide by this many semitones (0: none)...
        float bendFrom = 0.0f, bendTo = 0.0f; // ...from bendFrom to bendTo seconds after the start, straight in pitch
        float swellTo = 1.0f;       // the level moves in a straight line from the end of the attack...
        float swellUntil = 0.0f;    // ...to swellTo times the top by this many seconds after the start (0: none)
    };
}
