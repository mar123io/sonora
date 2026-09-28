#include "sonora/agent/planner.h"

#include <algorithm>
#include <array>
#include <cctype>

#include <nlohmann/json.hpp>

namespace sonora::agent {
namespace {

std::string Lowered(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return out;
}

bool Mentions(const std::string& lowered, std::string_view word) {
  return lowered.find(word) != std::string::npos;
}

// The subject of a sentence, for the small number of shapes this planner claims to read:
// whatever follows the verb. Returns empty when there is nothing after it.
std::string After(const std::string& lowered, std::string_view verb) {
  const std::size_t at = lowered.find(verb);
  if (at == std::string::npos) {
    return {};
  }
  std::size_t from = at + verb.size();
  while (from < lowered.size() &&
         (std::isspace(static_cast<unsigned char>(lowered[from])) != 0)) {
    ++from;
  }
  std::string subject = lowered.substr(from);
  while (!subject.empty() &&
         (std::isspace(static_cast<unsigned char>(subject.back())) != 0 ||
          subject.back() == '.' || subject.back() == '?' || subject.back() == '!')) {
    subject.pop_back();
  }
  return subject;
}

ToolCall Call(std::string tool, nlohmann::json arguments) {
  ToolCall call;
  call.tool = std::move(tool);
  call.arguments_json = arguments.dump();
  return call;
}

// The ids of the tracks a search returned, and nothing else from it.
//
// Only the ids. Not the titles, not the album names -- and that is the whole discipline of
// this function. A real provider will be shown the titles, because it has to be in order to
// choose; this one does not need them, so it does not read them, and what comes back from the
// library cannot reach a decision here at all.
std::vector<std::int64_t> TrackIds(const std::vector<Observation>& observations,
                                   std::size_t limit) {
  std::vector<std::int64_t> ids;
  for (const Observation& observation : observations) {
    if (observation.tool != "library.search" && observation.tool != "library.albumTracks" &&
        observation.tool != "library.artistTracks" &&
        observation.tool != "library.listTracks") {
      continue;
    }
    const auto parsed =
        nlohmann::json::parse(observation.result_json, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      continue;
    }
    const auto tracks = parsed.find("tracks");
    if (tracks == parsed.end() || !tracks->is_array()) {
      continue;
    }
    for (const auto& track : *tracks) {
      if (ids.size() >= limit) {
        return ids;
      }
      if (!track.is_object()) {
        continue;
      }
      const auto id = track.find("id");
      if (id != track.end() && id->is_number_integer()) {
        ids.push_back(id->get<std::int64_t>());
      }
    }
  }
  return ids;
}

Plan Unknown(std::string reply) {
  Plan plan;
  plan.understood = false;
  plan.reply = std::move(reply);
  return plan;
}

// The verbs this planner claims to read, in two languages because the person using it writes
// in one of them. Order matters only in that the first match wins.
struct Verb {
  std::string_view word;
  std::string_view tool;
};

constexpr std::array<Verb, 12> kTransport{{
    {"pause", "player.pause"},
    {"metti in pausa", "player.pause"},
    {"resume", "player.play"},
    {"riprendi", "player.play"},
    {"stop", "player.stop"},
    {"ferma", "player.stop"},
    {"next", "player.next"},
    {"skip", "player.next"},
    {"prossim", "player.next"},  // prossimo, prossima
    {"salta", "player.next"},
    {"previous", "player.previous"},
    {"precedent", "player.previous"},
}};

constexpr std::array<std::string_view, 6> kPlayVerbs{
    {"play ", "metti ", "riproduci ", "mettimi ", "fammi sentire ", "put on "}};

constexpr std::array<std::string_view, 5> kAskVerbs{{"what is playing", "what's playing",
                                                     "cosa sta suonando", "cosa sto ascoltando",
                                                     "che canzone"}};

}  // namespace

Plan LocalPlanner::Propose(const Request& request) {
  if (request.catalogue == nullptr || request.catalogue->empty()) {
    return Unknown("Nothing is available to do right now.");
  }
  const std::string lowered = Lowered(request.utterance);
  if (lowered.empty()) {
    return Unknown("Say what you would like.");
  }

  // Second round: a search has already happened, so the ids exist and the proposal is the
  // queue. The utterance is not consulted again -- what it asked for was decided last round.
  if (!request.observations.empty()) {
    const std::vector<std::int64_t> ids = TrackIds(request.observations, kMaxQueued);
    if (ids.empty()) {
      return Unknown("I did not find anything in your library for that.");
    }
    Plan plan;
    plan.understood = true;
    plan.reply = "Queue " + std::to_string(ids.size()) +
                 (ids.size() == 1 ? " track" : " tracks") + " and start playing.";
    plan.calls.push_back(Call("player.enqueue", {{"trackIds", ids}, {"replace", true}}));
    plan.calls.push_back(Call("player.play", nlohmann::json::object()));
    return plan;
  }

  for (const std::string_view phrase : kAskVerbs) {
    if (Mentions(lowered, phrase)) {
      Plan plan;
      plan.understood = true;
      plan.reply = "Reading what is playing.";
      plan.calls.push_back(Call("player.getState", nlohmann::json::object()));
      return plan;
    }
  }

  for (const Verb& verb : kTransport) {
    if (Mentions(lowered, verb.word)) {
      Plan plan;
      plan.understood = true;
      plan.reply = std::string("Propose ") + std::string(verb.tool) + ".";
      plan.calls.push_back(Call(std::string(verb.tool), nlohmann::json::object()));
      return plan;
    }
  }

  for (const std::string_view verb : kPlayVerbs) {
    const std::string subject = After(lowered, verb);
    if (subject.empty()) {
      continue;
    }
    Plan plan;
    plan.understood = true;
    // Said plainly, because it is what happens. This planner has matched a word; it has not
    // understood a mood, and telling somebody it did would make every other honest sentence
    // in this project worth less.
    plan.reply = "Searching your library for \"" + subject +
                 "\" — I match words, not moods; a real planner is what understands the rest.";
    plan.calls.push_back(Call("library.search", {{"query", subject}, {"limit", 50}}));
    return plan;
  }

  return Unknown(
      "I did not understand that. I know how to play or queue something by name, pause, "
      "resume, skip, and say what is playing.");
}

}  // namespace sonora::agent
