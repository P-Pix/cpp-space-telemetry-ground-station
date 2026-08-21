/**
 * @file CliParsing.hpp
 * @brief Fournit un parsing strict et réutilisable des valeurs de ligne de commande STGS.
 *
 * Les fonctions refusent les suffixes silencieux, les dépassements de capacité et les valeurs
 * flottantes non finies. Cette centralisation évite les conversions permissives de std::stoul/
 * std::stod qui peuvent accepter une chaîne partiellement valide.
 */

#pragma once

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace stgs
{

    /** Bornes des ports TCP/UDP utilisateurs : le port 0 est réservé à l'allocation automatique. */
    inline constexpr std::uint16_t MinimumNetworkPort = 1U;
    inline constexpr std::uint16_t MaximumNetworkPort = std::numeric_limits<std::uint16_t>::max();

    /** @brief Plage inclusive de ports validée par la CLI. */
    struct PortRange
    {
        std::uint16_t first = 0;
        std::uint16_t last = 0;

        /**
         * @brief Vérifie que la plage décrit au moins un port utilisateur valide.
         * @return true si `first` et `last` appartiennent à [1, 65535] et si `first <= last`.
         */
        [[nodiscard]] bool valid() const noexcept
        {
            return first >= MinimumNetworkPort && last >= first;
        }

        /**
         * @brief Retourne le nombre de ports de la plage inclusive.
         *
         * Une structure `PortRange` peut être construite directement sans passer par la CLI.
         * L'appel reste donc sûr sur une plage invalide et retourne 0 au lieu de provoquer un
         * sous-dépassement non signé. Les opérations réseau valident néanmoins la plage et lèvent
         * une exception afin de ne jamais masquer une configuration incorrecte.
         *
         * @return Nombre de ports lorsque la plage est valide, 0 sinon.
         */
        [[nodiscard]] std::size_t count() const noexcept
        {
            if (!valid())
            {
                return 0U;
            }
            return static_cast<std::size_t>(last) - static_cast<std::size_t>(first) + 1U;
        }
    };

    /**
     * @brief Parse un entier non signé en exigeant que toute la chaîne soit consommée.
     * @param value Représentation décimale sans suffixe.
     * @param name Nom d'option utilisé dans le diagnostic.
     * @param minimum Borne minimale incluse.
     * @param maximum Borne maximale incluse.
     * @return Valeur convertie dans le type demandé.
     * @throws std::runtime_error Si la syntaxe ou les bornes sont invalides.
     */
    template <typename UInt>
    UInt parseUnsigned(std::string_view value,
                       std::string_view name,
                       UInt minimum = 0,
                       UInt maximum = std::numeric_limits<UInt>::max())
    {
        static_assert(std::is_integral_v<UInt> && std::is_unsigned_v<UInt>,
                      "parseUnsigned requires an unsigned integral type");
        if (value.empty())
        {
            throw std::runtime_error(std::string(name) + " requires a decimal integer");
        }

        unsigned long long parsed = 0;
        const auto *begin = value.data();
        const auto *end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed, 10);
        if (result.ec != std::errc{} || result.ptr != end)
        {
            throw std::runtime_error(std::string(name) + " contains an invalid integer: " + std::string(value));
        }
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<UInt>::max()))
        {
            throw std::runtime_error(std::string(name) + " exceeds the target integer range");
        }
        const auto converted = static_cast<UInt>(parsed);
        if (converted < minimum || converted > maximum)
        {
            throw std::runtime_error(std::string(name) + " is outside the allowed range");
        }
        return converted;
    }

    /**
     * @brief Parse un flottant fini en refusant toute portion de chaîne non consommée.
     *
     * `strtod()` est utilisé pour conserver la prise en charge de la notation scientifique, mais
     * son pointeur de fin est contrôlé afin qu'une valeur telle que `12.5foo` ne soit pas acceptée
     * partiellement. NaN, les infinis et les dépassements signalés par `ERANGE` sont refusés pour
     * empêcher leur propagation dans les calculs de télémétrie et dans les exports JSON.
     *
     * @param value Représentation textuelle complète du nombre à convertir.
     * @param name Nom de l'option utilisé dans le message de diagnostic.
     * @return Valeur `double` finie obtenue après validation complète.
     * @throws std::runtime_error Si la chaîne est vide, partiellement invalide, hors plage, NaN ou infinie.
     */
    inline double parseFiniteDouble(std::string_view value, std::string_view name)
    {
        if (value.empty())
        {
            throw std::runtime_error(std::string(name) + " requires a number");
        }
        std::string owned(value);
        char *end = nullptr;
        errno = 0;
        const double parsed = std::strtod(owned.c_str(), &end);
        if (errno == ERANGE || end == owned.c_str() || end != owned.c_str() + owned.size() || !std::isfinite(parsed))
        {
            throw std::runtime_error(std::string(name) + " contains an invalid finite number: " + owned);
        }
        return parsed;
    }

    /**
     * @brief Parse une probabilité bornée dans l'intervalle fermé [0, 1].
     * @param value Représentation textuelle de la probabilité.
     * @param name Nom de l'option utilisé dans le diagnostic.
     * @return Probabilité finie comprise entre 0 et 1, bornes incluses.
     * @throws std::runtime_error Si la valeur n'est pas un flottant fini ou sort de [0, 1].
     */
    inline double parseProbability(std::string_view value, std::string_view name)
    {
        const double parsed = parseFiniteDouble(value, name);
        if (parsed < 0.0 || parsed > 1.0)
        {
            throw std::runtime_error(std::string(name) + " must be between 0 and 1");
        }
        return parsed;
    }

    /**
     * @brief Parse une plage de ports sous la forme `debut:fin` ou un port unique.
     * @param value Plage utilisateur.
     * @param name Nom d'option pour le diagnostic.
     * @return Plage normalisée avec bornes inclusives.
     * @throws std::runtime_error Si la syntaxe est incorrecte, si un port vaut 0, dépasse 65535
     * ou si la borne de début est supérieure à la borne de fin.
     */
    inline PortRange parsePortRange(std::string_view value, std::string_view name)
    {
        const auto separator = value.find(':');
        if (separator == std::string_view::npos)
        {
            const auto port = parseUnsigned<std::uint16_t>(value, name, MinimumNetworkPort, MaximumNetworkPort);
            return PortRange{port, port};
        }
        if (value.find(':', separator + 1U) != std::string_view::npos)
        {
            throw std::runtime_error(std::string(name) + " must use start:end syntax");
        }
        const auto first = parseUnsigned<std::uint16_t>(value.substr(0U, separator), name, MinimumNetworkPort, MaximumNetworkPort);
        const auto last = parseUnsigned<std::uint16_t>(value.substr(separator + 1U), name, MinimumNetworkPort, MaximumNetworkPort);
        if (first > last)
        {
            throw std::runtime_error(std::string(name) + " start port cannot exceed end port");
        }
        return PortRange{first, last};
    }

} // namespace stgs
