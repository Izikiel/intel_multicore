Guia de Estilo para el Kernel
=============================

La siguiente es una guia de estilo del codigo del Kernel. Con ella se
evaluaran los _commits_ de codigo realizados.

Son abiertas a discusion excepto donde se indica, 
y puede haber secciones del codigo actual que no la cumplan. Avisen y se 
modifica esas secciones del codigo para que cumplan la guía.

Guia de estilo
--------------

0. 80 caracteres por linea, 4 espacios por tab. Esto si no se discute.
1. Evite _assembly_ tanto como pueda. Si se puede escribir en C SE DEBE escribir
en C. Escribir en _assembly_ tiene extremos problemas. 
Las excepciones aceptables deben ser pensadas con mucho cuidado.
2. Usar ANSI C99. Especialmente estudien la bibliografía que incluyo al final
porque hay cosas MUY importantes para usar como las macros variadicas, los
iniciadores de structs, etc.
3. Se pueden agregar warnings a los CFLAGS. Pero no se pueden sacar. MUCHO 
MENOS sacar el -Werror. Ignorar los warnings es buscar bardo. Los warnings
actuales son suficientes y realmente el compilador esta en modo gorra.
4. Si un puntero no debe ser modificado, PONER SI O SI  _const_. Si un valor
es polleado por el codigo, PONER SI O SI _volatile_.
5. En lo potencial, escribir el codigo tal que toda función pueda ser probada
separada de lo demas. Más aun, en lo posible todo módulo que contenga lógica 
no relacionada con hardware DEBE SER testeado aparte (despues voy a definir 
como hacer esto).
6. Usar nombres largos y declarativos. Usar constantes con nombre y no números
magicos. Escribir funciones cortas (que entre en una pantalla de editor es el
limite, menos de 30 lineas es deseable). Evitar ifs o switchs largos usando 
_table driven dispatching_.
7. Comentar el código. Si se escribe código en base a una fuente, incluir la
fuente (link o pagina de libro). Toda linea no obvia debe tener su comentario.
Esto NO ES SUBSTITUTO de escribir código claro o nombres declarativos.
8. Si algo NO DEBE PASAR o DEBE PASAR, ponga un fail_if o un fail_unless. Fallar
lo antes posible evita banda de quilombos. Escriba tests usando fail_if y
fail_unless para código que tenga lógica (no tiene sentido para codigo de 
inicializacion de hardware, pero si para un filesystem o inicializador de 
memoria). _ASSUME NOTHING_ como dice Mike Abash.
9. Todo tipo de datos importante se debe declarar en algun .h
10. Toda función o simbolo que no se debe exportar se debe declarar static.
11. DELEGAR TANTO COMO SE PUEDA AL COMPILADOR. ES MUY DIFICIL TESTEAR UN KERNEL.
12. Todo el codigo esta en ingles. Los comentarios en español (preferentemente).
13. Respete las licencias. No incluya codigo GPL o LGPL porque esto es MIT. 
Incluya fuente de origen (link, libro) de todo codigo que se copie.

Fuentes
-------

* _21st Century C_ de Ben Klemens
* _Learn C The Hard Way_ de Zed Shaw (<http://c.learncodethehardway.org/book/>).
* <https://www.kernel.org/doc/Documentation/CodingStyle>, pero que me la soben
con lo de 4 espacios por tab. Todo lo demas es mega razonable. Ante la duda de
algo que no este, seguir lo que diga aca.
* <https://github.com/mcinglis/c-style>, muy buenos tips. 
