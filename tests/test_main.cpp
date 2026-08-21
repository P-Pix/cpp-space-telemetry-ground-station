/**
 * @file test_main.cpp
 * @brief Point d'entrée du mini-harnais de tests STGS.
 *
 * Les cas sont répartis par domaine afin que ce fichier ne contienne que l'enregistrement et le
 * rendu des résultats. Les tests réseau restent limités au loopback IPv4.
 */
#include "support/TestSupport.hpp"

#include <exception>
#include <iostream>

int main()
{
    stgs::test::TestRegistry tests;
    stgs::test::registerCodecTests(tests);
    stgs::test::registerApplicationSignalTests(tests);
    stgs::test::registerReplayHealthTests(tests);
    stgs::test::registerNetworkTests(tests);

    int failed = 0;
    for (const auto &[name, function] : tests)
    {
        try
        {
            function();
            std::cout << "[PASS] " << name << '\n';
        }
        catch (const std::exception &ex)
        {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    }

    if (failed != 0)
    {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test(s) passed\n";
    return 0;
}
