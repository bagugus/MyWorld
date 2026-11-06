#include "sim/Names.h"

#include <array>

namespace lw {
namespace {

constexpr std::array kFirstNames{
    "James",   "John",    "Robert",  "Michael", "William", "David",   "Richard", "Joseph",
    "Thomas",  "Charles", "Daniel",  "Matthew", "Anthony", "Mark",    "Steven",  "Paul",
    "Andrew",  "Joshua",  "Kevin",   "Brian",   "George",  "Edward",  "Ronald",  "Timothy",
    "Jason",   "Jeffrey", "Ryan",    "Jacob",   "Gary",    "Nicholas", "Eric",   "Jonathan",
    "Stephen", "Larry",   "Justin",  "Scott",   "Frank",   "Brandon", "Raymond", "Gregory",
    "Samuel",  "Patrick", "Jack",    "Dennis",  "Jerry",   "Tyler",   "Aaron",   "Henry",
    "Adam",    "Peter",   "Nathan",  "Walter",  "Harold",  "Carl",    "Arthur",  "Roger",
    "Victor",  "Leo",     "Owen",    "Lucas",   "Ethan",   "Simon",   "Oscar",   "Hugo",
    "Mary",    "Patricia", "Jennifer", "Linda", "Elizabeth", "Barbara", "Susan", "Jessica",
    "Sarah",   "Karen",   "Nancy",   "Lisa",    "Betty",   "Margaret", "Sandra", "Ashley",
    "Emily",   "Donna",   "Michelle", "Carol",  "Amanda",  "Melissa", "Deborah", "Laura",
    "Rebecca", "Sharon",  "Cynthia", "Kathleen", "Amy",    "Anna",    "Rachel",  "Helen",
    "Emma",    "Olivia",  "Grace",   "Alice",   "Julia",   "Claire",  "Hannah",  "Rose",
    "Lucy",    "Ruth",    "Diana",   "Joan",    "Evelyn",  "Martha",  "Megan",   "Kate",
    "Nora",    "Ellen",   "Vera",    "Irene",   "Molly",   "Sophie",  "Chloe",   "Zoe",
};

constexpr std::array kLastNames{
    "Smith",    "Johnson",  "Williams", "Brown",    "Jones",    "Miller",   "Davis",
    "Wilson",   "Anderson", "Taylor",   "Thomas",   "Moore",    "Martin",   "Jackson",
    "Thompson", "White",    "Harris",   "Clark",    "Lewis",    "Robinson", "Walker",
    "Young",    "Allen",    "King",     "Wright",   "Scott",    "Hill",     "Green",
    "Adams",    "Baker",    "Nelson",   "Carter",   "Mitchell", "Roberts",  "Turner",
    "Phillips", "Campbell", "Parker",   "Evans",    "Edwards",  "Collins",  "Stewart",
    "Morris",   "Rogers",   "Reed",     "Cook",     "Morgan",   "Bell",     "Murphy",
    "Bailey",   "Cooper",   "Richardson", "Cox",    "Howard",   "Ward",     "Peterson",
    "Gray",     "James",    "Watson",   "Brooks",   "Kelly",    "Sanders",  "Price",
    "Bennett",  "Wood",     "Barnes",   "Ross",     "Henderson", "Coleman", "Jenkins",
    "Perry",    "Powell",   "Long",     "Patterson", "Hughes",  "Flores",   "Washington",
    "Butler",   "Simmons",  "Foster",   "Bryant",   "Alexander", "Russell", "Griffin",
    "Hayes",    "Myers",    "Ford",     "Hamilton", "Graham",   "Sullivan", "Wallace",
    "West",     "Cole",     "Hunt",     "Black",    "Stone",    "Fisher",   "Hunter",
    "Fox",      "Lane",     "Webb",     "Mason",    "Dixon",    "Shaw",     "Holmes",
    "Rice",     "Hart",     "Porter",   "Burke",    "Walsh",    "Doyle",    "Quinn",
    "Barker",   "Fletcher", "Harper",   "Booth",    "Marsh",    "Sharp",    "Frost",
    "Knight",   "Carver",   "Lowell",   "Briggs",   "Hale",     "Pratt",    "Weston",
};

} // namespace

std::string_view randomFirstName(Rng& rng) {
    return kFirstNames[rng.below(static_cast<std::uint32_t>(kFirstNames.size()))];
}

std::string_view randomLastName(Rng& rng) {
    return kLastNames[rng.below(static_cast<std::uint32_t>(kLastNames.size()))];
}

bool isFemaleName(std::string_view first) {
    // Первые kMaleNames имён списка — мужские, остальные — женские.
    constexpr std::size_t kMaleNames = 64;
    static_assert(kFirstNames[kMaleNames - 1][0] == 'H' && kFirstNames[kMaleNames][0] == 'M');
    for (std::size_t i = kMaleNames; i < kFirstNames.size(); ++i) {
        if (first == kFirstNames[i]) {
            return true;
        }
    }
    return false;
}

std::string labelOf(const entt::registry& registry, entt::entity entity, EntityId id) {
    std::string label;
    if (entity != entt::null && registry.valid(entity)) {
        if (const auto* name = registry.try_get<Name>(entity)) {
            label = name->first + " " + name->last + " ";
        }
    }
    label += "#" + std::to_string(id);
    return label;
}

} // namespace lw
